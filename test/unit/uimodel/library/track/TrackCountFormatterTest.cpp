// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/library/track/TrackCountFormatter.h>

#include <catch2/catch_test_macros.hpp>

namespace ao::uimodel::test
{
  TEST_CASE("formatTrackCount distinguishes singular and plural track counts", "[uimodel][unit][library][track]")
  {
    CHECK(formatTrackCount(0) == "0 tracks");
    CHECK(formatTrackCount(1) == "1 track");
    CHECK(formatTrackCount(2) == "2 tracks");
    CHECK(formatTrackCount(42) == "42 tracks");
  }
} // namespace ao::uimodel::test
