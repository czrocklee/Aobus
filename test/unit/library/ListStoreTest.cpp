// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/library/ListStore.h>

#include "lib/library/ListRecordValidation.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/library/LibraryStoreTestSupport.h"
#include "test/unit/library/WritableLibraryTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/library/ListBuilder.h>
#include <ao/library/ListView.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

namespace ao::library::test
{
  namespace
  {
    template<typename Writer>
    concept HasRawListCreate = requires(Writer& writer) { writer.create(std::span<std::byte const>{}); };

    template<typename Writer>
    concept HasRawListUpdate = requires(Writer& writer) { writer.update(ListId{1}, std::span<std::byte const>{}); };

    static_assert(!HasRawListCreate<ListStore::Writer>);
    static_assert(!HasRawListUpdate<ListStore::Writer>);
    std::pair<ListId, ListView> requireCreate(ListStore::Writer writer, ListBuilder::Prepared const& prepared)
    {
      auto result = writer.create(prepared);
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

    auto const prepared = ao::test::requireValue(ListBuilder::makeEmpty().name("Stored").prepare());

    auto wtxn2 = writeTransaction(library);
    auto const [id, view] = requireCreate(physicalWriter(store, wtxn2), prepared);
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

    auto const prepared = ao::test::requireValue(builder.prepare());

    auto wtxn2 = writeTransaction(library);
    auto const [id, view] = requireCreate(physicalWriter(store, wtxn2), prepared);
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

    auto const prepared = ao::test::requireValue(ListBuilder::makeEmpty().prepare());

    auto wtxn2 = writeTransaction(library);
    auto const [id, view] = requireCreate(physicalWriter(store, wtxn2), prepared);
    REQUIRE(wtxn2.commit());

    // Delete it
    auto wtxn3 = writeTransaction(library);
    REQUIRE(physicalWriter(store, wtxn3).remove(id));
    REQUIRE(wtxn3.commit());

    // Verify it's gone
    auto rtxn = library.readTransaction();
    auto reader = store.reader(rtxn);
    auto it = reader.begin();
    CHECK(it == reader.end());
  }

  TEST_CASE("ListStore - invalid candidates cannot reach the prepared writer", "[library][regression][list]")
  {
    auto fixture = LibraryStoreFixture{};
    auto invalidBuilder = ListBuilder::makeEmpty();
    invalidBuilder.orderTrackIds().add(kInvalidTrackId);
    auto const invalidRes = invalidBuilder.prepare();
    REQUIRE_FALSE(invalidRes);
    CHECK(invalidRes.error().code == Error::Code::CorruptData);

    auto const original = ao::test::requireValue(ListBuilder::makeEmpty().name("Original").prepare());
    auto const updated = ao::test::requireValue(ListBuilder::makeEmpty().name("Updated").prepare());
    auto transaction = writeTransaction(fixture.library);
    auto writer = physicalWriter(fixture.library.lists(), transaction);

    auto createdRes = writer.create(original);
    REQUIRE(createdRes);
    REQUIRE(writer.update(*createdRes, updated));
    REQUIRE(transaction.commit());

    auto readTransaction = fixture.library.readTransaction();
    auto reader = fixture.library.lists().reader(readTransaction);
    auto const optStored = reader.get(*createdRes);
    REQUIRE(optStored);
    CHECK(optStored->name() == "Updated");
    CHECK(validateSerializedList(optStored->rawData()));
    auto iterator = reader.begin();
    REQUIRE(iterator != reader.end());
    CHECK((*iterator).first == *createdRes);
    ++iterator;
    CHECK(iterator == reader.end());
  }
} // namespace ao::library::test
