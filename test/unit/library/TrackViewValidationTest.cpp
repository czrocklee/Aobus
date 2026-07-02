// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "test/unit/library/TrackViewTestSupport.h"
#include <ao/library/TrackView.h>
#include <ao/utility/ByteView.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <span>

namespace ao::library::test
{
  TEST_CASE("TrackView - validates hot buffers", "[library][unit][track][validation]")
  {
    auto const data = makeMinimalHotTrackViewData();
    auto const view = TrackView{data, std::span<std::byte const>{}};
    CHECK(view.isHotValid() == true);
  }

  TEST_CASE("TrackView - rejects null hot data", "[library][unit][track][validation]")
  {
    auto const nullSpan = std::span<std::byte const>{static_cast<std::byte const*>(nullptr), 100};
    auto const nullView = TrackView{nullSpan, std::span<std::byte const>{}};
    CHECK(nullView.isHotValid() == false);
  }

  TEST_CASE("TrackView - rejects undersized hot data", "[library][unit][track][validation]")
  {
    auto const smallData = std::array<char, 10>{};
    auto const smallView = TrackView{utility::bytes::view(smallData), std::span<std::byte const>{}};
    CHECK(smallView.isHotValid() == false);
  }

  TEST_CASE("TrackView - validates cold buffers", "[library][unit][track][validation]")
  {
    auto const data = makeColdTrackViewData();
    auto const view = TrackView{std::span<std::byte const>{}, data};
    CHECK(view.isColdValid() == true);
  }

  TEST_CASE("TrackView - rejects null cold data", "[library][unit][track][validation]")
  {
    auto const nullSpan = std::span<std::byte const>{static_cast<std::byte const*>(nullptr), 100};
    auto const nullView = TrackView{std::span<std::byte const>{}, nullSpan};
    CHECK(nullView.isColdValid() == false);
  }

  TEST_CASE("TrackView - rejects undersized cold data", "[library][unit][track][validation]")
  {
    auto const smallData = std::array<char, 10>{};
    auto const smallView = TrackView{std::span<std::byte const>{}, utility::bytes::view(smallData)};
    CHECK(smallView.isColdValid() == false);
  }
} // namespace ao::library::test
