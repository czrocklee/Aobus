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

    std::pair<ListId, ListView> requireCreate(ListStore::Writer writer, ListBuilder::Prepared const& prepared)
    {
      auto res = writer.create(prepared);
      REQUIRE(res);
      auto optView = writer.get(*res);
      REQUIRE(optView);
      return {*res, *optView};
    }
  } // namespace

  TEST_CASE("ListStore - create and read", "[library][unit][list]")
  {
    auto fixture = LibraryStoreFixture{};
    auto& library = fixture.library;
    auto const& store = library.lists();

    auto const prepared = ao::test::requireValue(ListBuilder::makeEmpty()
                                                   .name("Stored")
                                                   .description("Testing smart list round-trip")
                                                   .filter("@year > 2020")
                                                   .prepare());

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
    CHECK((*it).second.description() == "Testing smart list round-trip");
    CHECK((*it).second.filter() == "@year > 2020");
    CHECK((*it).second.orderTrackIds().empty());
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

  TEST_CASE("ListStore - non-contiguous saved order round-trips", "[library][unit][list]")
  {
    auto fixture = LibraryStoreFixture{};
    auto& library = fixture.library;
    auto const& store = library.lists();

    auto builder = ListBuilder::makeEmpty().name("RoundTrip Test").description("Testing round-trip");
    builder.orderTrackIds().add(TrackId{42});
    builder.orderTrackIds().add(TrackId{99});
    auto const prepared = ao::test::requireValue(builder.prepare());

    auto wtxn2 = writeTransaction(library);
    auto const [id, createdView] = requireCreate(physicalWriter(store, wtxn2), prepared);
    REQUIRE(wtxn2.commit());

    auto rtxn = library.readTransaction();
    auto const optFoundResult = store.reader(rtxn).get(id);
    REQUIRE(optFoundResult);

    auto const& found = *optFoundResult;
    CHECK(found.name() == "RoundTrip Test");
    REQUIRE(found.orderTrackIds().size() == 2);
    CHECK(found.orderTrackIds()[0] == TrackId{42});
    CHECK(found.orderTrackIds()[1] == TrackId{99});
  }

  TEST_CASE("ListStore - delete removes only the selected record", "[library][unit][list]")
  {
    auto fixture = LibraryStoreFixture{};
    auto& library = fixture.library;
    auto const& store = library.lists();

    auto const prepared = ao::test::requireValue(ListBuilder::makeEmpty().name("Target").prepare());
    auto const survivor = ao::test::requireValue(ListBuilder::makeEmpty().name("Survivor").prepare());

    auto wtxn2 = writeTransaction(library);
    auto const [id, view] = requireCreate(physicalWriter(store, wtxn2), prepared);
    auto const [survivorId, survivorView] = requireCreate(physicalWriter(store, wtxn2), survivor);
    REQUIRE(wtxn2.commit());

    // Delete it
    auto wtxn3 = writeTransaction(library);
    REQUIRE(physicalWriter(store, wtxn3).tryRemove(id));
    REQUIRE(wtxn3.commit());

    // Verify it's gone
    auto rtxn = library.readTransaction();
    auto reader = store.reader(rtxn);
    CHECK_FALSE(reader.get(id));
    auto const optSurvivor = reader.get(survivorId);
    REQUIRE(optSurvivor);
    CHECK(optSurvivor->name() == "Survivor");
  }

  TEST_CASE("ListStore - prepared-only writes persist a canonical replacement", "[library][unit][list]")
  {
    STATIC_REQUIRE_FALSE(HasRawListCreate<ListStore::Writer>);
    STATIC_REQUIRE_FALSE(HasRawListUpdate<ListStore::Writer>);
    auto fixture = LibraryStoreFixture{};

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
