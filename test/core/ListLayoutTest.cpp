// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <catch2/catch.hpp>

#include <cstddef>
#include <rs/core/ListLayout.h>
#include <rs/core/ListPayloadBuilder.h>
#include <rs/core/Type.h>
#include <span>
#include <test/core/TestUtils.h>

#include <vector>

namespace
{
  using namespace test;
  using rs::core::ListHeader;
  using rs::core::ListView;

  TEST_CASE("ListHeader - Size and Alignment")
  {
    CHECK(sizeof(ListHeader) == 16);
    CHECK(alignof(ListHeader) == 4);
  }

  TEST_CASE("ListHeader - Field Offsets")
  {
    CHECK(offsetof(ListHeader, trackIdsCount) == 0);
    CHECK(offsetof(ListHeader, nameOffset) == 4);
    CHECK(offsetof(ListHeader, nameLen) == 6);
    CHECK(offsetof(ListHeader, descOffset) == 8);
    CHECK(offsetof(ListHeader, descLen) == 10);
    CHECK(offsetof(ListHeader, filterOffset) == 12);
    CHECK(offsetof(ListHeader, filterLen) == 14);
  }

  TEST_CASE("ListView - Default Constructor")
  {
    auto view = ListView{};
    CHECK(view.isValid() == false);
  }

  TEST_CASE("ListView - Construct from Data")
  {
    auto payload = rs::core::ListPayloadBuilder::buildManualList("", "", {});
    auto view = ListView{std::as_bytes(std::span{payload})};

    CHECK(view.isValid() == true);
    CHECK(view.trackIdsCount() == 0);
  }

  TEST_CASE("ListView - Field Accessors")
  {
    auto payload = rs::core::ListPayloadBuilder::buildManualList("Test", "Desc", {});
    auto view = ListView{std::as_bytes(std::span{payload})};

    CHECK(view.trackIdsCount() == 0);
    CHECK(view.name() == "Test");
    CHECK(view.description() == "Desc");
    CHECK(view.filter().empty());
    CHECK(view.isSmart() == false);
  }

  TEST_CASE("ListView - Manual List with TrackIds")
  {
    std::array<rs::core::TrackId, 3> const trackIds = {
      rs::core::TrackId{100},
      rs::core::TrackId{200},
      rs::core::TrackId{300}
    };
    auto payload = rs::core::ListPayloadBuilder::buildManualList("My List", "Description", trackIds);
    auto view = ListView{std::as_bytes(std::span{payload})};

    CHECK(view.trackIdsCount() == 3);
    CHECK(view.name() == "My List");
    CHECK(view.description() == "Description");
    CHECK(view.isSmart() == false);
    CHECK(view.trackIds()[0] == rs::core::TrackId{100});
    CHECK(view.trackIds()[1] == rs::core::TrackId{200});
    CHECK(view.trackIds()[2] == rs::core::TrackId{300});
  }

  TEST_CASE("ListView - Smart List with Filter")
  {
    auto payload = rs::core::ListPayloadBuilder::buildSmartList(
        "Smart List", "A smart list", "@year > 2020");
    auto view = ListView{std::as_bytes(std::span{payload})};

    CHECK(view.trackIdsCount() == 0);
    CHECK(view.name() == "Smart List");
    CHECK(view.description() == "A smart list");
    CHECK(view.filter() == "@year > 2020");
    CHECK(view.isSmart() == true);
  }

  TEST_CASE("ListView - Empty Strings")
  {
    auto payload = rs::core::ListPayloadBuilder::buildManualList("", "", {});
    auto view = ListView{std::as_bytes(std::span{payload})};

    CHECK(view.name().empty());
    CHECK(view.description().empty());
    CHECK(view.filter().empty());
  }

  TEST_CASE("ListView - Invalid Data")
  {
    std::span<std::byte const> nullSpan{static_cast<std::byte const*>(nullptr), 100};
    auto nullView = ListView{nullSpan};
    CHECK(nullView.isValid() == false);

    char smallData[10] = {};
    std::span<std::byte const> smallSpan{reinterpret_cast<std::byte const*>(smallData), sizeof(smallData)};
    auto smallView = ListView{smallSpan};
    CHECK(smallView.isValid() == false);
  }

  TEST_CASE("ListView - isSmart")
  {
    auto manualPayload = rs::core::ListPayloadBuilder::buildManualList("Manual", "", {});
    auto manualView = ListView{std::as_bytes(std::span{manualPayload})};
    CHECK(manualView.isSmart() == false);

    auto smartPayload = rs::core::ListPayloadBuilder::buildSmartList("Smart", "", "@year > 2020");
    auto smartView = ListView{std::as_bytes(std::span{smartPayload})};
    CHECK(smartView.isSmart() == true);
  }

  TEST_CASE("ListView - Large trackIds Count")
  {
    auto payload = rs::core::ListPayloadBuilder::buildManualList("Test", "", {});
    auto view = ListView{std::as_bytes(std::span{payload})};
    CHECK(view.trackIds().empty());
  }

} // anonymous namespace
