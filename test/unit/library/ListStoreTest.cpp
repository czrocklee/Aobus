// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/library/ListStore.h>

#include "test/unit/TestFixtureSupport.h"
#include "test/unit/library/LibraryStoreTestSupport.h"
#include "test/unit/library/WritableLibraryTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/library/ListBuilder.h>
#include <ao/library/ListView.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

namespace ao::library::test
{
  namespace
  {
    std::pair<ListId, ListView> requireCreate(ListStore::Writer writer, std::span<std::byte const> data)
    {
      auto result = writer.create(data);
      REQUIRE(result);
      auto optView = writer.get(*result);
      REQUIRE(optView);
      return {*result, *optView};
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

    auto const createRes = writer.create(corruptPayload);
    REQUIRE_FALSE(createRes);
    CHECK(createRes.error().code == Error::Code::CorruptData);

    auto createdRes = writer.create(validPayload);
    REQUIRE(createdRes);
    auto const updateRes = writer.update(*createdRes, corruptPayload);
    REQUIRE_FALSE(updateRes);
    CHECK(updateRes.error().code == Error::Code::CorruptData);
    REQUIRE(transaction.commit());

    auto readTransaction = fixture.library.readTransaction();
    auto reader = fixture.library.lists().reader(readTransaction);
    auto const optStored = reader.get(*createdRes);
    REQUIRE(optStored);
    CHECK(optStored->name() == "Original");
    auto iterator = reader.begin();
    REQUIRE(iterator != reader.end());
    CHECK((*iterator).first == *createdRes);
    ++iterator;
    CHECK(iterator == reader.end());
  }
} // namespace ao::library::test
