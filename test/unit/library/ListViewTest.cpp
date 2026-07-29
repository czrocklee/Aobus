// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/TestUtils.h"
#include <ao/CoreIds.h>
#include <ao/library/ListBuilder.h>
#include <ao/library/ListLayout.h>
#include <ao/library/ListView.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <vector>

namespace ao::library::test
{
  TEST_CASE("ListView - constructs from serialized data", "[library][unit][list]")
  {
    auto const payload = ao::test::requireValue(ListBuilder::makeEmpty().serialize());
    auto const view = ListView{payload};
    CHECK(view.orderTrackIds().empty());
  }

  TEST_CASE("ListView - returns serialized field values", "[library][unit][list]")
  {
    auto const payload =
      ao::test::requireValue(ListBuilder::makeEmpty().name("Test").description("Desc").parentId(ListId{9}).serialize());
    auto const view = ListView{payload};

    CHECK(view.orderTrackIds().empty());
    CHECK(view.name() == "Test");
    CHECK(view.description() == "Desc");
    CHECK(view.filter().empty());
    CHECK(view.parentId() == ListId{9});
    CHECK(view.isRootParent() == false);
  }

  TEST_CASE("ListView - returns stored order track IDs", "[library][unit][list]")
  {
    auto builder = ListBuilder::makeEmpty().name("My List").description("Description");
    builder.orderTrackIds().add(TrackId{100});
    builder.orderTrackIds().add(TrackId{200});
    builder.orderTrackIds().add(TrackId{300});
    auto const payload = ao::test::requireValue(builder.serialize());
    auto const view = ListView{payload};

    CHECK(view.orderTrackIds().size() == 3);
    CHECK_FALSE(view.orderTrackIds().empty());
    CHECK(view.name() == "My List");
    CHECK(view.description() == "Description");
    CHECK(view.orderTrackIds()[0] == TrackId{100});
    CHECK(view.orderTrackIds()[1] == TrackId{200});
    CHECK(view.orderTrackIds()[2] == TrackId{300});
  }

  TEST_CASE("ListView - returns local expressions", "[library][unit][list]")
  {
    auto const payload = ao::test::requireValue(
      ListBuilder::makeEmpty().name("Smart List").description("A smart list").filter("@year > 2020").serialize());
    auto const view = ListView{payload};

    CHECK(view.orderTrackIds().empty());
    CHECK(view.name() == "Smart List");
    CHECK(view.description() == "A smart list");
    CHECK(view.filter() == "@year > 2020");
  }

  TEST_CASE("ListView - returns empty strings when lengths are zero", "[library][unit][list]")
  {
    auto const payload = ao::test::requireValue(ListBuilder::makeEmpty().serialize());
    auto const view = ListView{payload};

    CHECK(view.name().empty());
    CHECK(view.description().empty());
    CHECK(view.filter().empty());
    CHECK(view.isRootParent() == true);
  }

  TEST_CASE("ListView - poisons invalid serialized data", "[library][unit][list]")
  {
    auto checkPoisoned = [](ListView const& view)
    {
      CHECK_FALSE(view.isValid());
      CHECK(view.name().empty());
      CHECK(view.description().empty());
      CHECK(view.filter().empty());
      CHECK(view.orderTrackIds().empty());
      CHECK(view.parentId() == kInvalidListId);
    };

    auto const nullSpan = std::span<std::byte const>{static_cast<std::byte*>(nullptr), 100};
    checkPoisoned(ListView{nullSpan});

    auto const smallData = std::vector<std::byte>(10);
    checkPoisoned(ListView{smallData});

    SECTION("track-id array overruns the record")
    {
      auto data = std::vector<std::byte>(kListHeaderSize, std::byte{0});
      auto header = ListHeader{};
      header.orderTrackIdCount = 4;
      std::memcpy(data.data(), &header, sizeof(ListHeader));
      checkPoisoned(ListView{data});
    }

    SECTION("string extent overruns the record")
    {
      auto data = std::vector<std::byte>(kListHeaderSize, std::byte{0});
      auto header = ListHeader{};
      header.nameLength = 16;
      std::memcpy(data.data(), &header, sizeof(ListHeader));
      checkPoisoned(ListView{data});
    }
  }

  TEST_CASE("ListView - valid records report isValid", "[library][unit][list]")
  {
    auto const payload = ao::test::requireValue(ListBuilder::makeEmpty().name("Test").serialize());
    CHECK(ListView{payload}.isValid());
  }

  TEST_CASE("ListView - rejects trailing bytes and nonzero padding", "[library][unit][list]")
  {
    auto trailingPayload = ao::test::requireValue(ListBuilder::makeEmpty().name("Aligned").serialize());
    trailingPayload.insert(trailingPayload.end(), 4, std::byte{0});
    CHECK_FALSE(ListView{trailingPayload}.isValid());

    auto nonzeroPaddingPayload = ao::test::requireValue(ListBuilder::makeEmpty().name("x").serialize());
    nonzeroPaddingPayload.back() = std::byte{1};
    CHECK_FALSE(ListView{nonzeroPaddingPayload}.isValid());
  }

  TEST_CASE("ListView - rejects an unaligned record base", "[library][regression][list]")
  {
    auto const payload = ao::test::requireValue(ListBuilder::makeEmpty().name("Test").serialize());
    auto backing = std::vector<std::byte>(payload.size() + 1);
    std::ranges::copy(payload, backing.begin() + 1);
    auto const unaligned = std::span<std::byte const>{backing.data() + 1, payload.size()};

    CHECK_FALSE(ListView{unaligned}.isValid());
  }

  TEST_CASE("ListView - rejects a track count that overflows a 32-bit host", "[library][regression][list]")
  {
    if constexpr (sizeof(std::size_t) == 4)
    {
      auto data = std::vector<std::byte>(kListHeaderSize, std::byte{0});
      auto header = ListHeader{};
      header.orderTrackIdCount = std::numeric_limits<std::uint32_t>::max();
      std::memcpy(data.data(), &header, sizeof(header));
      CHECK_FALSE(ListView{data}.isValid());
    }
  }
} // namespace ao::library::test
