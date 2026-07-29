// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/TestFixtureSupport.h"
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

    std::vector<std::byte> duplicateOrderPayload(std::span<TrackId const> trackIds)
    {
      auto const trackIdsSize = trackIds.size_bytes();
      auto const header = ListHeader{
        .orderTrackIdCount = static_cast<std::uint32_t>(trackIds.size()),
      };

      auto result = std::vector<std::byte>{};
      result.reserve(sizeof(header) + trackIdsSize);
      result.insert_range(result.end(), utility::bytes::view(header));
      result.insert_range(result.end(), utility::bytes::view(trackIds));
      return result;
    }
  } // namespace

  TEST_CASE("ListBuilder - expression and order coexist in one record", "[library][unit][list]")
  {
    auto builder = ListBuilder::makeEmpty()
                     .name("My Saved List")
                     .description("An ordered expression")
                     .filter("@artist = 'Test'")
                     .parentId(ListId{17});
    builder.orderTrackIds().add(TrackId{31}).add(TrackId{12});
    auto const payload = ao::test::requireValue(builder.serialize());
    auto const view = ListView{payload};

    CHECK(view.name() == "My Saved List");
    CHECK(view.filter() == "@artist = 'Test'");
    CHECK(view.parentId() == ListId{17});
    REQUIRE(view.orderTrackIds().size() == 2);
    CHECK(view.orderTrackIds()[0] == TrackId{31});
    CHECK(view.orderTrackIds()[1] == TrackId{12});
  }

  TEST_CASE("ListBuilder - zeroes every alignment padding byte", "[library][regression][list]")
  {
    auto const payload = ao::test::requireValue(ListBuilder::makeEmpty().name("x").serialize());
    constexpr auto kLogicalSize = kListHeaderSize + 1;
    REQUIRE(payload.size() == 24);

    for (auto const byte : std::span{payload}.subspan(kLogicalSize))
    {
      CHECK(byte == std::byte{0});
    }
  }

  TEST_CASE("ListBuilder - order track IDs round-trip", "[library][unit][list]")
  {
    auto builder = ListBuilder::makeEmpty().name("My List").description("A saved order");
    builder.orderTrackIds().add(TrackId{100});
    builder.orderTrackIds().add(TrackId{200});
    builder.orderTrackIds().add(TrackId{300});
    auto const payload = ao::test::requireValue(builder.serialize());
    auto const view = ListView{payload};

    CHECK(view.name() == "My List");
    CHECK(view.orderTrackIds().size() == 3);
    CHECK(view.orderTrackIds()[0] == TrackId{100});
    CHECK(view.orderTrackIds()[1] == TrackId{200});
    CHECK(view.orderTrackIds()[2] == TrackId{300});
  }

  TEST_CASE("ListBuilder - add retains only the first occurrence in request order", "[library][unit][list]")
  {
    auto builder = ListBuilder::makeEmpty();
    builder.orderTrackIds().add(TrackId{30}).add(TrackId{10}).add(TrackId{30}).add(TrackId{20}).add(TrackId{10});

    auto const payload = ao::test::requireValue(builder.serialize());
    auto const view = ListView{payload};

    REQUIRE(view.orderTrackIds().size() == 3);
    CHECK(view.orderTrackIds()[0] == TrackId{30});
    CHECK(view.orderTrackIds()[1] == TrackId{10});
    CHECK(view.orderTrackIds()[2] == TrackId{20});
  }

  TEST_CASE("ListBuilder - remove eliminates an ID after repeated add requests", "[library][unit][list]")
  {
    auto builder = ListBuilder::makeEmpty();
    builder.orderTrackIds().add(TrackId{10}).add(TrackId{20}).add(TrackId{10}).add(TrackId{30});

    builder.orderTrackIds().remove(TrackId{10});

    auto const payload = ao::test::requireValue(builder.serialize());
    auto const view = ListView{payload};
    REQUIRE(view.orderTrackIds().size() == 2);
    CHECK(view.orderTrackIds()[0] == TrackId{20});
    CHECK(view.orderTrackIds()[1] == TrackId{30});
  }

  TEST_CASE("ListBuilder - fromView canonicalizes duplicate order IDs by first occurrence", "[library][unit][list]")
  {
    auto const duplicateTrackIds = std::array{TrackId{30}, TrackId{10}, TrackId{30}, TrackId{20}, TrackId{10}};
    auto const duplicatePayload = duplicateOrderPayload(duplicateTrackIds);
    auto const duplicateView = ListView{duplicatePayload};
    REQUIRE(duplicateView.isValid());
    REQUIRE(duplicateView.orderTrackIds().size() == 5);

    auto const rebuiltPayload = ao::test::requireValue(ListBuilder::fromView(duplicateView).serialize());
    auto const rebuiltView = ListView{rebuiltPayload};

    REQUIRE(rebuiltView.orderTrackIds().size() == 3);
    CHECK(rebuiltView.orderTrackIds()[0] == TrackId{30});
    CHECK(rebuiltView.orderTrackIds()[1] == TrackId{10});
    CHECK(rebuiltView.orderTrackIds()[2] == TrackId{20});
  }

  TEST_CASE("ListBuilder - empty expression and order round-trip", "[library][unit][list]")
  {
    auto const payload =
      ao::test::requireValue(ListBuilder::makeEmpty().name("Empty List").description("No tracks").serialize());
    auto const view = ListView{payload};

    CHECK(view.orderTrackIds().empty());
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

  TEST_CASE("ListBuilder - saved order round-trips through ListStore", "[library][unit][list]")
  {
    auto fixture = LibraryStoreFixture{};
    auto& library = fixture.library;
    auto const& store = library.lists();

    auto builder = ListBuilder::makeEmpty().name("RoundTrip Test").description("Testing round-trip");
    builder.orderTrackIds().add(TrackId{42});
    builder.orderTrackIds().add(TrackId{99});
    auto const payload = ao::test::requireValue(builder.serialize());

    auto wtxn2 = writeTransaction(library);
    auto const [id, createdView] = requireCreate(store.writer(wtxn2), payload);
    REQUIRE(wtxn2.commit());

    auto rtxn = library.readTransaction();
    auto const optFoundResult = store.reader(rtxn).get(id);
    REQUIRE(optFoundResult);

    auto const& found = *optFoundResult;
    CHECK(found.name() == "RoundTrip Test");
    CHECK(found.orderTrackIds().size() == 2);
    CHECK(found.orderTrackIds()[0] == TrackId{42});
    CHECK(found.orderTrackIds()[1] == TrackId{99});
  }

  TEST_CASE("ListBuilder - expression round-trips through ListStore", "[library][unit][list]")
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
    CHECK(found.name() == "Smart RoundTrip");
    CHECK(found.filter() == "@year > 2020");
    CHECK(found.orderTrackIds().empty());
  }

  TEST_CASE("ListBuilder - derives adjacent text field positions", "[library][unit][list]")
  {
    auto const payload =
      ao::test::requireValue(ListBuilder::makeEmpty().name("Offset Test").description("Desc Here").serialize());
    auto const view = ListView{payload};

    CHECK(view.name() == "Offset Test");
    CHECK(view.description() == "Desc Here");
  }

  TEST_CASE("ListBuilder - serialization rejects text beyond the product limit", "[library][unit][list]")
  {
    auto const longTextResult = ListBuilder::makeEmpty().name(std::string(65'536, 'n')).serialize();
    REQUIRE_FALSE(longTextResult);
    CHECK(longTextResult.error().code == Error::Code::ValueTooLarge);
  }

  TEST_CASE("ListBuilder - order supports more than 16-bit byte offsets", "[library][unit][list]")
  {
    auto builder = ListBuilder::makeEmpty();

    for (std::uint32_t rawId = 1; rawId <= 20'000; ++rawId)
    {
      builder.orderTrackIds().add(TrackId{rawId});
    }

    auto const payload = ao::test::requireValue(
      builder.name("Beyond legacy offset").description("Still canonical").filter("#ordered").serialize());
    auto const view = ListView{payload};

    REQUIRE(view.isValid());
    REQUIRE(view.orderTrackIds().size() == 20'000);
    CHECK(view.orderTrackIds()[0] == TrackId{1});
    CHECK(view.orderTrackIds()[19'999] == TrackId{20'000});
    CHECK(view.name() == "Beyond legacy offset");
    CHECK(view.description() == "Still canonical");
    CHECK(view.filter() == "#ordered");
  }
} // namespace ao::library::test
