// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tui/TrackSection.h"

#include <catch2/catch_test_macros.hpp>

namespace ao::tui::test
{
  TEST_CASE("TrackSection - display names fall back consistently", "[tui][unit][track-section]")
  {
    CHECK(trackSectionDisplayName(TrackSection{.primaryText = "Album A"}) == "Album A");
    CHECK(trackSectionDisplayName(TrackSection{}) == "Untitled Section");
  }
} // namespace ao::tui::test
