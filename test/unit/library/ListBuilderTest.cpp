// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "test/unit/TestUtils.h"
#include "test/unit/library/LibraryStoreTestSupport.h"
#include "test/unit/library/WritableLibraryTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/library/ListBuilder.h>
#include <ao/library/ListLayout.h>
#include <ao/library/ListStore.h>
#include <ao/library/ListView.h>
#include <ao/utility/ByteView.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

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

    std::vector<std::byte> legacyListPayload(std::span<TrackId const> trackIds)
    {
      auto const trackIdsSize = trackIds.size_bytes();
      auto const stringOffset = static_cast<std::uint16_t>(trackIdsSize);
      auto const header = ListHeader{
        .trackIdCount = static_cast<std::uint32_t>(trackIds.size()),
        .nameOffset = stringOffset,
        .descOffset = stringOffset,
        .filterOffset = stringOffset,
      };

      auto result = std::vector<std::byte>{};
      result.reserve(sizeof(header) + trackIdsSize);
      result.insert_range(result.end(), utility::bytes::view(header));
      result.insert_range(result.end(), utility::bytes::view(trackIds));
      return result;
    }
  } // namespace

  TEST_CASE("ListBuilder - smart list", "[library][unit][list]")
  {
    auto const payload = ao::test::requireValue(ListBuilder::makeEmpty()
                                                  .name("My Smart List")
                                                  .description("A smart list")
                                                  .filter("@artist = 'Test'")
                                                  .parentId(ListId{17})
                                                  .serialize());
    auto const view = ListView{payload};

    CHECK(view.isSmart() == true);
    CHECK(view.name() == "My Smart List");
    CHECK(view.filter() == "@artist = 'Test'");
    CHECK(view.parentId() == ListId{17});
  }

  TEST_CASE("ListBuilder - manual list", "[library][unit][list]")
  {
    auto builder = ListBuilder::makeEmpty().name("My Manual List").description("A manual list");
    builder.tracks().add(TrackId{100});
    builder.tracks().add(TrackId{200});
    builder.tracks().add(TrackId{300});
    auto const payload = ao::test::requireValue(builder.serialize());
    auto const view = ListView{payload};

    CHECK(view.isSmart() == false);
    CHECK(view.name() == "My Manual List");
    CHECK(view.tracks().size() == 3);
    CHECK(view.tracks()[0] == TrackId{100});
    CHECK(view.tracks()[1] == TrackId{200});
    CHECK(view.tracks()[2] == TrackId{300});
  }

  TEST_CASE("ListBuilder - add retains only the first occurrence in request order", "[library][unit][list]")
  {
    auto builder = ListBuilder::makeEmpty();
    builder.tracks().add(TrackId{30}).add(TrackId{10}).add(TrackId{30}).add(TrackId{20}).add(TrackId{10});

    auto const payload = ao::test::requireValue(builder.serialize());
    auto const view = ListView{payload};

    REQUIRE(view.tracks().size() == 3);
    CHECK(view.tracks()[0] == TrackId{30});
    CHECK(view.tracks()[1] == TrackId{10});
    CHECK(view.tracks()[2] == TrackId{20});
  }

  TEST_CASE("ListBuilder - remove eliminates membership after repeated add requests", "[library][unit][list]")
  {
    auto builder = ListBuilder::makeEmpty();
    builder.tracks().add(TrackId{10}).add(TrackId{20}).add(TrackId{10}).add(TrackId{30});

    builder.tracks().remove(TrackId{10});

    auto const payload = ao::test::requireValue(builder.serialize());
    auto const view = ListView{payload};
    REQUIRE(view.tracks().size() == 2);
    CHECK(view.tracks()[0] == TrackId{20});
    CHECK(view.tracks()[1] == TrackId{30});
  }

  TEST_CASE("ListBuilder - fromView canonicalizes legacy duplicates by first occurrence", "[library][unit][list]")
  {
    auto const legacyTrackIds = std::array{TrackId{30}, TrackId{10}, TrackId{30}, TrackId{20}, TrackId{10}};
    auto const legacyPayload = legacyListPayload(legacyTrackIds);
    auto const legacyView = ListView{legacyPayload};
    REQUIRE(legacyView.isValid());
    REQUIRE(legacyView.tracks().size() == 5);

    auto const rebuiltPayload = ao::test::requireValue(ListBuilder::fromView(legacyView).serialize());
    auto const rebuiltView = ListView{rebuiltPayload};

    REQUIRE(rebuiltView.tracks().size() == 3);
    CHECK(rebuiltView.tracks()[0] == TrackId{30});
    CHECK(rebuiltView.tracks()[1] == TrackId{10});
    CHECK(rebuiltView.tracks()[2] == TrackId{20});
  }

  TEST_CASE("ListBuilder - manual list empty trackIds", "[library][unit][list]")
  {
    auto const payload =
      ao::test::requireValue(ListBuilder::makeEmpty().name("Empty List").description("No tracks").serialize());
    auto const view = ListView{payload};

    CHECK(view.isSmart() == false);
    CHECK(view.tracks().empty());
    CHECK(view.parentId() == kInvalidListId);
  }

  TEST_CASE("ListBuilder - parentId round-trip through View", "[library][unit][list]")
  {
    auto builder = ListBuilder::makeEmpty()
                     .name("Nested Smart List")
                     .description("Child list")
                     .filter("$year >= 2021")
                     .parentId(ListId{42});

    auto const payload = ao::test::requireValue(builder.serialize());
    auto const view = ListView{payload};

    CHECK(view.parentId() == ListId{42});

    auto const rebuilt = ao::test::requireValue(ListBuilder::fromView(view).serialize());
    auto const rebuiltView = ListView{rebuilt};
    CHECK(rebuiltView.parentId() == ListId{42});
    CHECK(rebuiltView.name() == "Nested Smart List");
    CHECK(rebuiltView.filter() == "$year >= 2021");
  }

  TEST_CASE("ListBuilder - manual list round-trip through ListStore", "[library][unit][list]")
  {
    auto fixture = LibraryStoreFixture{};
    auto& library = fixture.library;
    auto const& store = library.lists();

    auto builder = ListBuilder::makeEmpty().name("RoundTrip Test").description("Testing round-trip");
    builder.tracks().add(TrackId{42});
    builder.tracks().add(TrackId{99});
    auto const payload = ao::test::requireValue(builder.serialize());

    auto wtxn2 = writeTransaction(library);
    auto const [id, createdView] = requireCreate(store.writer(wtxn2), payload);
    REQUIRE(wtxn2.commit());

    auto rtxn = library.readTransaction();
    auto const optFoundResult = store.reader(rtxn).get(id);
    REQUIRE(optFoundResult);

    auto const& found = *optFoundResult;
    CHECK(found.isSmart() == false);
    CHECK(found.name() == "RoundTrip Test");
    CHECK(found.tracks().size() == 2);
    CHECK(found.tracks()[0] == TrackId{42});
    CHECK(found.tracks()[1] == TrackId{99});
  }

  TEST_CASE("ListBuilder - smart list round-trip through ListStore", "[library][unit][list]")
  {
    auto fixture = LibraryStoreFixture{};
    auto& library = fixture.library;
    auto const& store = library.lists();

    auto const payload = ao::test::requireValue(ListBuilder::makeEmpty()
                                                  .name("Smart RoundTrip")
                                                  .description("Testing smart list round-trip")
                                                  .filter("@year > 2020")
                                                  .serialize());

    auto wtxn2 = writeTransaction(library);
    auto const [id, createdView] = requireCreate(store.writer(wtxn2), payload);
    REQUIRE(wtxn2.commit());

    auto rtxn = library.readTransaction();
    auto const optFoundResult = store.reader(rtxn).get(id);
    REQUIRE(optFoundResult);

    auto const& found = *optFoundResult;
    CHECK(found.isSmart() == true);
    CHECK(found.name() == "Smart RoundTrip");
    CHECK(found.filter() == "@year > 2020");
    CHECK(found.tracks().empty());
  }

  TEST_CASE("ListBuilder - name and description offsets", "[library][unit][list]")
  {
    auto const payload =
      ao::test::requireValue(ListBuilder::makeEmpty().name("Offset Test").description("Desc Here").serialize());
    auto const view = ListView{payload};

    CHECK(view.name() == "Offset Test");
    CHECK(view.description() == "Desc Here");
  }

  TEST_CASE("ListBuilder - serialization rejects values that overflow its fixed-width layout", "[library][unit][list]")
  {
    auto const longTextResult = ListBuilder::makeEmpty().name(std::string(65'536, 'n')).serialize();
    REQUIRE_FALSE(longTextResult);
    CHECK(longTextResult.error().code == Error::Code::ValueTooLarge);

    auto builder = ListBuilder::makeEmpty();

    for (std::uint32_t rawId = 1; rawId <= 16'384; ++rawId)
    {
      builder.tracks().add(TrackId{rawId});
    }

    auto const trackCountResult = builder.serialize();
    REQUIRE_FALSE(trackCountResult);
    CHECK(trackCountResult.error().code == Error::Code::ValueTooLarge);
  }
} // namespace ao::library::test
