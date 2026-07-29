// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/TestFixtureSupport.h"
#include "test/unit/library/LibraryStoreTestSupport.h"
#include "test/unit/library/WritableLibraryTestSupport.h"
#include "test/unit/lmdb/LmdbTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/library/ListBuilder.h>
#include <ao/library/ListStore.h>
#include <ao/library/ListView.h>
#include <ao/library/detail/LibraryError.h>
#include <ao/lmdb/Environment.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <tuple>
#include <utility>

namespace ao::library::test
{
  namespace
  {
    std::pair<ListId, ListView> requireCreate(ListStore::Writer writer, std::span<std::byte const> data)
    {
      auto result = writer.create(data);
      REQUIRE(result);
      return *result;
    }

    void seedCorruptList(std::filesystem::path const& path)
    {
      using namespace ao::lmdb::test;

      auto environment = openEnvironment(path, {.flags = lmdb::kEnvNoTls, .maxDatabases = 8});
      auto transaction = beginWriteTransaction(environment);
      auto database = openDatabase(transaction, "lists");
      auto const corruptPayload = std::array<std::byte, 4>{};
      REQUIRE(database.writer(transaction).create(1, corruptPayload));
      REQUIRE(transaction.commit());
    }
  } // namespace

  TEST_CASE("ListStore - create and read", "[library][unit][list]")
  {
    auto fixture = LibraryStoreFixture{};
    auto& library = fixture.library;
    auto const& store = library.lists();

    auto const data = ao::test::requireValue(ListBuilder::makeEmpty().name("Stored").serialize());

    auto wtxn2 = writeTransaction(library);
    auto const [id, view] = requireCreate(store.writer(wtxn2), data);
    REQUIRE(wtxn2.commit());

    // Read the list
    auto rtxn = library.readTransaction();
    auto reader = store.reader(rtxn);
    auto it = reader.begin();
    REQUIRE(it != reader.end());
    CHECK((*it).first == id);
    CHECK((*it).second.name() == "Stored");
  }

  TEST_CASE("ListStore - read by id", "[library][unit][list]")
  {
    auto fixture = LibraryStoreFixture{};
    auto& library = fixture.library;
    auto const& store = library.lists();

    auto builder = ListBuilder::makeEmpty().name("Ordered");

    for (std::uint32_t rawId = 1; rawId <= 10; ++rawId)
    {
      builder.orderTrackIds().add(TrackId{rawId});
    }

    auto const data = ao::test::requireValue(builder.serialize());

    auto wtxn2 = writeTransaction(library);
    auto const [id, view] = requireCreate(store.writer(wtxn2), data);
    REQUIRE(wtxn2.commit());

    // Read by ID
    auto rtxn = library.readTransaction();
    auto const optFound = store.reader(rtxn).get(id);
    REQUIRE(optFound);
    REQUIRE(optFound->orderTrackIds().size() == 10);
    CHECK(optFound->orderTrackIds()[0] == TrackId{1});
    CHECK(optFound->orderTrackIds()[9] == TrackId{10});
  }

  TEST_CASE("ListStore - delete", "[library][unit][list]")
  {
    auto fixture = LibraryStoreFixture{};
    auto& library = fixture.library;
    auto const& store = library.lists();

    auto const data = ao::test::requireValue(ListBuilder::makeEmpty().serialize());

    auto wtxn2 = writeTransaction(library);
    auto const [id, view] = requireCreate(store.writer(wtxn2), data);
    REQUIRE(wtxn2.commit());

    // Delete it
    auto wtxn3 = writeTransaction(library);
    REQUIRE(store.writer(wtxn3).remove(id));
    REQUIRE(wtxn3.commit());

    // Verify it's gone
    auto rtxn = library.readTransaction();
    auto reader = store.reader(rtxn);
    auto it = reader.begin();
    CHECK(it == reader.end());
  }

  TEST_CASE("ListStore - rejects structurally invalid records before mutation", "[library][regression][list]")
  {
    auto fixture = LibraryStoreFixture{};
    auto const validPayload = ao::test::requireValue(ListBuilder::makeEmpty().name("Original").serialize());
    auto const corruptPayload = std::array<std::byte, 4>{};
    auto transaction = writeTransaction(fixture.library);
    auto writer = fixture.library.lists().writer(transaction);

    auto const createResult = writer.create(corruptPayload);
    REQUIRE_FALSE(createResult);
    CHECK(createResult.error().code == Error::Code::CorruptData);

    auto created = writer.create(validPayload);
    REQUIRE(created);
    auto const updateResult = writer.update(created->first, corruptPayload);
    REQUIRE_FALSE(updateResult);
    CHECK(updateResult.error().code == Error::Code::CorruptData);
    REQUIRE(transaction.commit());

    auto readTransaction = fixture.library.readTransaction();
    auto reader = fixture.library.lists().reader(readTransaction);
    auto const optStored = reader.get(created->first);
    REQUIRE(optStored);
    CHECK(optStored->name() == "Original");
    auto iterator = reader.begin();
    REQUIRE(iterator != reader.end());
    CHECK((*iterator).first == created->first);
    ++iterator;
    CHECK(iterator == reader.end());
  }

  TEST_CASE("ListStore - corrupt records fail closed for point reads and iteration", "[library][regression][list]")
  {
    auto const temp = ao::test::TempDir{};
    seedCorruptList(temp.path());
    auto library = MusicLibrary{temp.path(), temp.path()};

    SECTION("reader get")
    {
      auto transaction = library.readTransaction();

      try
      {
        std::ignore = library.lists().reader(transaction).get(ListId{1});
        FAIL("corrupt List point read did not throw");
      }
      catch (detail::LibraryException const& error)
      {
        CHECK(error.error().code == Error::Code::CorruptData);
      }
    }

    SECTION("writer get")
    {
      auto transaction = writeTransaction(library);

      try
      {
        std::ignore = library.lists().writer(transaction).get(ListId{1});
        FAIL("corrupt List writer point read did not throw");
      }
      catch (detail::LibraryException const& error)
      {
        CHECK(error.error().code == Error::Code::CorruptData);
      }
    }

    SECTION("iterator dereference")
    {
      auto transaction = library.readTransaction();
      auto iterator = library.lists().reader(transaction).begin();

      try
      {
        std::ignore = *iterator;
        FAIL("corrupt List iteration did not throw");
      }
      catch (detail::LibraryException const& error)
      {
        CHECK(error.error().code == Error::Code::CorruptData);
      }
    }
  }
} // namespace ao::library::test
