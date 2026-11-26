// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/winui/track/TrackSelectionNotifications.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <limits>
#include <vector>

namespace ao::winui::test
{
  namespace
  {
    using Kind = TrackSelectionEventKind;
    using Span = TrackSelectionSpan;
    using Events = std::vector<TrackSelectionEvent>;

    constexpr auto kLastIndex = std::numeric_limits<std::uint32_t>::max();
  } // namespace

  TEST_CASE("Track selection notifications - unchanged ranges produce no events")
  {
    auto const before = std::array{Span{1, kLastIndex}};
    auto const after = before;

    CHECK(trackSelectionNotifications({}, {}).empty());
    CHECK(trackSelectionNotifications(before, after).empty());
  }

  TEST_CASE("Track selection notifications - a new sole item reports selection instead of addition")
  {
    auto const after = std::array{Span{7, 1}};

    SECTION("Selecting from empty has no removal")
    {
      CHECK(trackSelectionNotifications({}, after) == Events{{Kind::Selected, 7}});
    }

    SECTION("Replacing a single item reports its removal after selection")
    {
      auto const before = std::array{Span{10, 1}};

      CHECK(trackSelectionNotifications(before, after) == Events{{Kind::Selected, 7}, {Kind::Removed, 10}});
    }

    SECTION("Replacing several items reports only the selected item")
    {
      auto const before = std::array{Span{10, 2}};

      CHECK(trackSelectionNotifications(before, after) == Events{{Kind::Selected, 7}});
    }
  }

  TEST_CASE("Track selection notifications - retaining one item does not select it again")
  {
    auto const before = std::array{Span{6, 3}};
    auto const after = std::array{Span{7, 1}};

    CHECK(trackSelectionNotifications(before, after) == Events{{Kind::Removed, 6}, {Kind::Removed, 8}});
  }

  TEST_CASE("Track selection notifications - clearing selection reports removals")
  {
    auto const before = std::array{Span{6, 2}, Span{10, 1}};

    CHECK(trackSelectionNotifications(before, {}) ==
          Events{{Kind::Removed, 6}, {Kind::Removed, 7}, {Kind::Removed, 10}});
  }

  TEST_CASE("Track selection notifications - overlapping ranges report additions before removals")
  {
    auto const before = std::array{Span{2, 5}, Span{9, 2}};
    auto const after = std::array{Span{1, 3}, Span{5, 5}};

    CHECK(trackSelectionNotifications(before, after) ==
          Events{{Kind::Added, 1}, {Kind::Added, 7}, {Kind::Added, 8}, {Kind::Removed, 4}, {Kind::Removed, 10}});
  }

  TEST_CASE("Track selection notifications - twenty total changes fit but twenty-one invalidate")
  {
    auto const before = std::array{Span{1, 10}};

    SECTION("Twenty changes are reported individually")
    {
      auto const after = std::array{Span{11, 10}};
      auto const events = trackSelectionNotifications(before, after);

      REQUIRE(events.size() == 20);
      CHECK(events.front() == TrackSelectionEvent{Kind::Added, 11});
      CHECK(events[9] == TrackSelectionEvent{Kind::Added, 20});
      CHECK(events[10] == TrackSelectionEvent{Kind::Removed, 1});
      CHECK(events.back() == TrackSelectionEvent{Kind::Removed, 10});
    }

    SECTION("The limit combines additions and removals")
    {
      auto const after = std::array{Span{11, 11}};

      CHECK(trackSelectionNotifications(before, after) == Events{{Kind::Invalidated, 0}});
    }
  }

  TEST_CASE("Track selection notifications - a huge overlap can change only the last representable index")
  {
    auto const before = std::array{Span{1, kLastIndex - 1}};
    auto const after = std::array{Span{1, kLastIndex}};

    CHECK(trackSelectionNotifications(before, after) == Events{{Kind::Added, kLastIndex}});
    CHECK(trackSelectionNotifications(after, before) == Events{{Kind::Removed, kLastIndex}});
  }

  TEST_CASE("Track selection notifications - bulk replacement invalidates even with a newly selected sole item")
  {
    auto const before = std::array{Span{0, kLastIndex}};
    auto const after = std::array{Span{kLastIndex, 1}};

    CHECK(trackSelectionNotifications(before, after) == Events{{Kind::Invalidated, 0}});
  }
} // namespace ao::winui::test
