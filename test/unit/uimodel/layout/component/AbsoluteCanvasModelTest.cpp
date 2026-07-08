// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/layout/component/AbsoluteCanvasModel.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <vector>

namespace ao::uimodel::test
{
  TEST_CASE("AbsoluteCanvasModel - applies canvas geometry policy", "[uimodel][unit][layout][component]")
  {
    SECTION("z ordering uses z-index before insertion order")
    {
      auto items = std::vector<AbsoluteCanvasItem>{
        {.id = "middle", .rect = {.x = 10, .y = 20, .width = 80, .height = 40}, .zIndex = 2, .insertOrder = 1},
        {.id = "front", .rect = {.x = 10, .y = 20, .width = 80, .height = 40}, .zIndex = 5, .insertOrder = 0},
        {.id = "back", .rect = {.x = 10, .y = 20, .width = 80, .height = 40}, .zIndex = 2, .insertOrder = 0}};

      std::ranges::stable_sort(items,
                               [](AbsoluteCanvasItem const& itemA, AbsoluteCanvasItem const& itemB)
                               { return ordersAbsoluteCanvasBefore(itemA, itemB); });

      REQUIRE(items.size() == 3);
      CHECK(items[0].id == "back");
      CHECK(items[1].id == "middle");
      CHECK(items[2].id == "front");
    }

    SECTION("hit testing returns the topmost item under a point")
    {
      auto const items = std::vector<AbsoluteCanvasItem>{
        {.id = "base", .rect = {.x = 10, .y = 20, .width = 100, .height = 70}, .zIndex = 2, .insertOrder = 4},
        {.id = "top", .rect = {.x = 30, .y = 40, .width = 40, .height = 30}, .zIndex = 7, .insertOrder = 1},
        {.id = "outside", .rect = {.x = 200, .y = 40, .width = 40, .height = 30}, .zIndex = 12, .insertOrder = 2}};

      auto const optHit = hitTestAbsoluteCanvas(items, 35, 45);

      REQUIRE(optHit);
      CHECK(items[*optHit].id == "top");
      CHECK(!hitTestAbsoluteCanvas(items, 180, 45).has_value());
    }

    SECTION("hit testing breaks equal z-index ties by insertion order")
    {
      auto const items = std::vector<AbsoluteCanvasItem>{
        {.id = "first", .rect = {.x = 5, .y = 5, .width = 80, .height = 80}, .zIndex = 3, .insertOrder = 1},
        {.id = "second", .rect = {.x = 5, .y = 5, .width = 80, .height = 80}, .zIndex = 3, .insertOrder = 2}};

      auto const optHit = hitTestAbsoluteCanvas(items, 20, 20);

      REQUIRE(optHit);
      CHECK(items[*optHit].id == "second");
    }

    SECTION("resize corner detection uses a small corner hit target")
    {
      CHECK(detectAbsoluteCanvasResizeCorner(120, 80, 4.0, 5.0) == AbsoluteCanvasResizeCorner::TopLeft);
      CHECK(detectAbsoluteCanvasResizeCorner(120, 80, 117.0, 6.0) == AbsoluteCanvasResizeCorner::TopRight);
      CHECK(detectAbsoluteCanvasResizeCorner(120, 80, 7.0, 76.0) == AbsoluteCanvasResizeCorner::BottomLeft);
      CHECK(detectAbsoluteCanvasResizeCorner(120, 80, 116.0, 79.0) == AbsoluteCanvasResizeCorner::BottomRight);
      CHECK(detectAbsoluteCanvasResizeCorner(120, 80, 45.0, 30.0) == AbsoluteCanvasResizeCorner::None);
    }

    SECTION("snap rounds to the nearest grid line and tolerates disabled grids")
    {
      CHECK(snapAbsoluteCanvasValue(29, true, 8) == 32);
      CHECK(snapAbsoluteCanvasValue(27, true, 8) == 24);
      CHECK(snapAbsoluteCanvasValue(29, false, 8) == 29);
      CHECK(snapAbsoluteCanvasValue(29, true, 0) == 29);
    }

    SECTION("move drag keeps live coordinates raw and snaps only on commit")
    {
      auto const start = AbsoluteCanvasRect{.x = 13, .y = 21, .width = 90, .height = 45};

      auto const live = updateAbsoluteCanvasMoveDrag(start, 14, 18);
      CHECK(live.x == 27);
      CHECK(live.y == 39);
      CHECK(live.width == 90);
      CHECK(live.height == 45);

      auto const committed = commitAbsoluteCanvasMoveDrag(start, 14, 18, true, 8);
      CHECK(committed.x == 24);
      CHECK(committed.y == 40);
      CHECK(committed.width == 90);
      CHECK(committed.height == 45);
    }

    SECTION("top-left resize snaps origin and clamps dimensions to measured minimums")
    {
      auto const start = AbsoluteCanvasRect{.x = 50, .y = 70, .width = 120, .height = 90};

      auto const live =
        updateAbsoluteCanvasResizeDrag(start, AbsoluteCanvasResizeCorner::TopLeft, 98, 75, 64, 32, true, 8);

      CHECK(live.x == 152);
      CHECK(live.y == 144);
      CHECK(live.width == 64);
      CHECK(live.height == 32);
    }

    SECTION("top-right resize keeps the left edge fixed")
    {
      auto const start = AbsoluteCanvasRect{.x = 40, .y = 70, .width = 120, .height = 90};

      auto const live =
        updateAbsoluteCanvasResizeDrag(start, AbsoluteCanvasResizeCorner::TopRight, 17, 22, 60, 45, true, 8);

      CHECK(live.x == 40);
      CHECK(live.y == 96);
      CHECK(live.width == 137);
      CHECK(live.height == 68);
    }

    SECTION("bottom-left resize keeps the top edge fixed")
    {
      auto const start = AbsoluteCanvasRect{.x = 40, .y = 70, .width = 120, .height = 90};

      auto const live =
        updateAbsoluteCanvasResizeDrag(start, AbsoluteCanvasResizeCorner::BottomLeft, 18, 23, 60, 45, true, 8);

      CHECK(live.x == 56);
      CHECK(live.y == 70);
      CHECK(live.width == 102);
      CHECK(live.height == 113);
    }

    SECTION("bottom-right resize grows and commit snaps dimensions")
    {
      auto const start = AbsoluteCanvasRect{.x = 20, .y = 30, .width = 103, .height = 59};

      auto const live =
        updateAbsoluteCanvasResizeDrag(start, AbsoluteCanvasResizeCorner::BottomRight, 14, 9, 50, 30, true, 8);
      CHECK(live.x == 20);
      CHECK(live.y == 30);
      CHECK(live.width == 117);
      CHECK(live.height == 68);

      auto const committed = commitAbsoluteCanvasResizeDrag(live, true, 8);
      CHECK(committed.x == 24);
      CHECK(committed.y == 32);
      CHECK(committed.width == 120);
      CHECK(committed.height == 72);
    }

    SECTION("keyboard nudges use the grid step when snapping is enabled")
    {
      auto const start = AbsoluteCanvasRect{.x = 40, .y = 50, .width = 75, .height = 25};

      CHECK(nudgeAbsoluteCanvasRect(start, AbsoluteCanvasNudgeDirection::Left, true, 16).x == 24);
      CHECK(nudgeAbsoluteCanvasRect(start, AbsoluteCanvasNudgeDirection::Right, true, 16).x == 56);
      CHECK(nudgeAbsoluteCanvasRect(start, AbsoluteCanvasNudgeDirection::Up, false, 16).y == 49);
      CHECK(nudgeAbsoluteCanvasRect(start, AbsoluteCanvasNudgeDirection::Down, true, 16).y == 66);
    }
  }
} // namespace ao::uimodel::test
