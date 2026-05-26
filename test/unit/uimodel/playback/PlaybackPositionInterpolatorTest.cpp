// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/playback/PlaybackPositionInterpolator.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

namespace ao::uimodel::playback::test
{
  TEST_CASE("PlaybackPositionInterpolator - calculations", "[unit][uimodel][playback]")
  {
    auto interpolator = PlaybackPositionInterpolator{};

    SECTION("Initial state is zero and not playing")
    {
      CHECK(interpolator.isPlaying() == false);
      CHECK(interpolator.interpolate(1000) == 0);
    }

    SECTION("Interpolation while playing")
    {
      std::uint32_t const pos = 1000;
      std::uint32_t const dur = 5000;
      interpolator.updateState(pos, dur, true);

      CHECK(interpolator.isPlaying() == true);

      // First frame sets the base time
      std::int64_t const t0 = 1000000;
      CHECK(interpolator.interpolate(t0) == pos);

      // 500ms later (500,000 microseconds)
      CHECK(interpolator.interpolate(t0 + 500000) == pos + 500);

      // Past duration
      CHECK(interpolator.interpolate(t0 + 10000000) == dur);
    }

    SECTION("Static value while paused")
    {
      interpolator.updateState(2000, 5000, false);
      CHECK(interpolator.isPlaying() == false);
      CHECK(interpolator.interpolate(1000000) == 2000);
      CHECK(interpolator.interpolate(2000000) == 2000);
    }

    SECTION("Reset clears state")
    {
      interpolator.updateState(1000, 5000, true);
      interpolator.reset();
      CHECK(interpolator.isPlaying() == false);
      CHECK(interpolator.interpolate(1000000) == 0);
    }
  }
} // namespace ao::uimodel::playback::test
