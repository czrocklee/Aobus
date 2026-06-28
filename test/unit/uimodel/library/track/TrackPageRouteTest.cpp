// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/Type.h>
#include <ao/rt/CorePrimitives.h>
#include <ao/uimodel/library/track/TrackPageRoute.h>

#include <catch2/catch_test_macros.hpp>

#include <vector>

namespace ao::uimodel::test
{
  TEST_CASE("TrackPageRoute - describeSelectionRoute", "[unit][uimodel][track]")
  {
    SECTION("valid view")
    {
      auto const route = describeSelectionRoute(rt::ViewId{42}, {TrackId{1}, TrackId{2}});
      CHECK(route.focusedViewId == rt::ViewId{42});
      CHECK(route.selectedIds == std::vector<TrackId>{TrackId{1}, TrackId{2}});
      CHECK(route.shouldUpdateRuntimeSelection == true);
    }

    SECTION("invalid view")
    {
      auto const route = describeSelectionRoute(rt::kInvalidViewId, {TrackId{1}});
      CHECK(route.focusedViewId == rt::kInvalidViewId);
      CHECK(route.selectedIds == std::vector<TrackId>{TrackId{1}});
      CHECK(route.shouldUpdateRuntimeSelection == false);
    }
  }

  TEST_CASE("TrackPageRoute - smartListParentIdFromPage", "[unit][uimodel][track]")
  {
    CHECK(smartListParentIdFromPage(rt::kAllTracksListId) == kInvalidListId);
    CHECK(smartListParentIdFromPage(kInvalidListId) == kInvalidListId);
    CHECK(smartListParentIdFromPage(ListId{10}) == ListId{10});
  }
} // namespace ao::uimodel::test
