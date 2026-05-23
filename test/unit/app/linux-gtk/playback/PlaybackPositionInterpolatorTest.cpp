// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/PlaybackPositionInterpolator.h"

#include <catch2/catch_test_macros.hpp>

using namespace ao::gtk;

TEST_CASE("PlaybackPositionInterpolator - State handling", "[playback][interpolator]")
{
  auto interpolator = PlaybackPositionInterpolator{};

  SECTION("Initial state is empty")
  {
    REQUIRE(interpolator.isPlaying() == false);
    REQUIRE(interpolator.lastDurationMs() == 0);
    REQUIRE(interpolator.interpolate(1000000) == 0);
  }

  SECTION("Updates state correctly")
  {
    interpolator.updateState(5000, 30000, true);

    REQUIRE(interpolator.isPlaying() == true);
    REQUIRE(interpolator.lastDurationMs() == 30000);
  }

  SECTION("Reset clears state")
  {
    interpolator.updateState(5000, 30000, true);
    interpolator.reset();

    REQUIRE(interpolator.isPlaying() == false);
    REQUIRE(interpolator.lastDurationMs() == 0);
    REQUIRE(interpolator.interpolate(1000000) == 0);
  }
}

TEST_CASE("PlaybackPositionInterpolator - Interpolation logic", "[playback][interpolator]")
{
  auto interpolator = PlaybackPositionInterpolator{};

  SECTION("Returns last position when paused")
  {
    interpolator.updateState(5000, 30000, false);

    // Any frame time should return exactly 5000
    REQUIRE(interpolator.interpolate(1000000) == 5000);
    REQUIRE(interpolator.interpolate(2000000) == 5000);
  }

  SECTION("Advances position based on frame time delta")
  {
    interpolator.updateState(5000, 30000, true);

    // First call sets the baseline frame time, returns base position
    REQUIRE(interpolator.interpolate(1000000) == 5000); // 1,000,000 us = 1000 ms

    // Second call is 200ms later
    REQUIRE(interpolator.interpolate(1200000) == 5200);

    // Third call is 500ms later from start
    REQUIRE(interpolator.interpolate(1500000) == 5500);
  }

  SECTION("Caps interpolated position at duration")
  {
    interpolator.updateState(29000, 30000, true);

    // Baseline
    REQUIRE(interpolator.interpolate(1000000) == 29000);

    // 500ms later
    REQUIRE(interpolator.interpolate(1500000) == 29500);

    // 1500ms later -> would be 30500, should be capped at 30000
    REQUIRE(interpolator.interpolate(2500000) == 30000);
  }

  SECTION("Handles non-monotonic frame times safely")
  {
    interpolator.updateState(5000, 30000, true);

    // Baseline at 2 seconds
    REQUIRE(interpolator.interpolate(2000000) == 5000);

    // Time goes backwards to 1 second! Should return last pos and reset baseline.
    REQUIRE(interpolator.interpolate(1000000) == 5000);

    // Now moves forward from the new 1 second baseline
    REQUIRE(interpolator.interpolate(1500000) == 5500);
  }
}
