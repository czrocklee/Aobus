// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/library/ListBuilder.h>

#include "lib/library/ListRecordValidation.h"
#include "test/unit/TestFixtureSupport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/ListLayout.h>
#include <ao/library/ListView.h>
#include <ao/utility/ByteView.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ao::library::test
{
  namespace
  {
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

    std::vector<std::byte> rawNamePayload(std::string_view const name)
    {
      auto const header = ListHeader{.nameLength = static_cast<std::uint32_t>(name.size())};
      auto result = std::vector<std::byte>{};
      result.insert_range(result.end(), utility::bytes::view(header));
      result.insert_range(result.end(), utility::bytes::view(name));

      while (result.size() % kListHeaderAlignment != 0)
      {
        result.push_back(std::byte{0});
      }

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

  TEST_CASE("ListBuilder - normalizes display text", "[library][unit][list][unicode]")
  {
    auto const payload = ao::test::requireValue(
      ListBuilder::makeEmpty().name("Cafe\u0301").description("Cre\u0300me bru\u0302le\u0301e").serialize());
    auto const view = ListView{payload};

    CHECK(view.name() == "Café");
    CHECK(view.description() == "Crème brûlée");
    CHECK(validateSerializedList(payload));
  }

  TEST_CASE("ListBuilder - rejects malformed UTF-8 text", "[library][unit][list][unicode]")
  {
    auto const malformed = std::string{"\xC0\xAF", 2};
    auto const res = ListBuilder::makeEmpty().name(malformed).serialize();

    REQUIRE_FALSE(res);
    CHECK(res.error().code == Error::Code::InvalidInput);
    CHECK(res.error().message.contains("List name"));
  }

  TEST_CASE("ListBuilder - validation rejects persisted non-NFC text", "[library][unit][list][unicode]")
  {
    auto const payload = rawNamePayload("Cafe\u0301");
    auto const view = ListView{payload};
    auto const res = validateSerializedList(payload);

    REQUIRE(view.isValid());
    REQUIRE_FALSE(res);
    CHECK(res.error().code == Error::Code::CorruptData);
    CHECK(res.error().message.contains("List name"));
  }

  TEST_CASE("ListBuilder - zeroes every alignment padding byte", "[library][unit][list]")
  {
    auto const payload = ao::test::requireValue(ListBuilder::makeEmpty().name("x").serialize());
    constexpr auto kLogicalSize = kListHeaderSize + 1;
    REQUIRE(payload.size() == 24);

    for (auto const byte : std::span{payload}.subspan(kLogicalSize))
    {
      CHECK(byte == std::byte{0});
    }
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
    auto const validationRes = validateSerializedList(duplicatePayload);
    REQUIRE_FALSE(validationRes);
    CHECK(validationRes.error().code == Error::Code::CorruptData);

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

    CHECK(view.filter().empty());
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
    auto const longTextRes = ListBuilder::makeEmpty().name(std::string(65'536, 'n')).serialize();
    REQUIRE_FALSE(longTextRes);
    CHECK(longTextRes.error().code == Error::Code::ValueTooLarge);
  }

  TEST_CASE("ListBuilder - preparation preserves opaque filter bytes", "[library][unit][list]")
  {
    auto filter = std::string_view{};

    SECTION("query grammar is not interpreted")
    {
      filter = "((( this is not query grammar";
    }

    SECTION("filter text is not normalized")
    {
      filter = "$title = 'Re\u0301sume\u0301'";
    }

    auto const prepared = ao::test::requireValue(ListBuilder::makeEmpty().filter(filter).prepare());
    auto const view = ListView{prepared.bytes()};

    REQUIRE(view.isValid());
    CHECK(view.filter() == filter);
    CHECK(validateSerializedList(prepared.bytes()));
  }

  TEST_CASE("ListBuilder - builder and prepared values own text snapshots", "[library][unit][list]")
  {
    auto name = std::string{"Snapshot"};
    auto filter = std::string{"not valid query syntax (("};
    auto builder = ListBuilder::makeEmpty().name(name).filter(filter);

    name = "Source mutated";
    filter = "$title = source-changed";
    auto const prepared = ao::test::requireValue(builder.prepare());

    name = "Builder mutated";
    filter = "$title = builder-changed";
    builder.name(name).filter(filter);
    auto const view = ListView{prepared.bytes()};

    REQUIRE(view.isValid());
    CHECK(view.name() == "Snapshot");
    CHECK(view.filter() == "not valid query syntax ((");
    CHECK(validateSerializedList(prepared.bytes()));
  }

  TEST_CASE("ListBuilder - preparation rejects the reserved saved-order Track id", "[library][unit][list]")
  {
    auto builder = ListBuilder::makeEmpty();
    builder.orderTrackIds().add(TrackId{1});
    REQUIRE(builder.prepare());
    builder.orderTrackIds().clear().add(kInvalidTrackId);

    auto const preparedRes = builder.prepare();

    REQUIRE_FALSE(preparedRes);
    CHECK(preparedRes.error().code == Error::Code::CorruptData);
    CHECK(preparedRes.error().message == "List record contains the reserved Track id zero");
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
