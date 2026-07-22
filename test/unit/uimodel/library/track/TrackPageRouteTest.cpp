// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/CoreIds.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/uimodel/library/track/TrackPageRoute.h>

#include <catch2/catch_test_macros.hpp>

namespace ao::uimodel::test
{
  TEST_CASE("TrackPageRoute - smartListParentIdFromPage", "[uimodel][unit][track]")
  {
    CHECK(smartListParentIdFromPage(rt::kAllTracksListId) == kInvalidListId);
    CHECK(smartListParentIdFromPage(kInvalidListId) == kInvalidListId);
    CHECK(smartListParentIdFromPage(ListId{10}) == ListId{10});
  }
} // namespace ao::uimodel::test
