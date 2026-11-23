// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/FrameClock.h>
#include <ao/uimodel/playback/seek/PlaybackPositionInteraction.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>

namespace ao::uimodel::test
{
  TEST_CASE("PlaybackPositionInterpolator - calculations")
  {
    auto interpolator = PlaybackPositionInterpolator{};

    SECTION("Initial state is zero and not playing")
    {
      CHECK(interpolator.lastDuration() == std::chrono::milliseconds{0});
      CHECK(interpolator.isPlaying() == false);
      CHECK(interpolator.interpolateElapsed(FrameClock::fromMicros(1000)) == std::chrono::milliseconds{0});
    }

    SECTION("Interpolation while playing")
    {
      auto const elapsed = std::chrono::seconds{1};
      auto const duration = std::chrono::seconds{5};
      interpolator.updateState(elapsed, duration, true);

      CHECK(interpolator.isPlaying() == true);

      // First frame establishes the base timestamp and reports the known elapsed.
      auto const startTime = FrameClock::fromMicros(1'000'000);
      CHECK(interpolator.interpolateElapsed(startTime) == elapsed);

      // 500ms later
      CHECK(interpolator.interpolateElapsed(startTime + std::chrono::milliseconds{500}) ==
            elapsed + std::chrono::milliseconds{500});

      // Past duration is clamped
      CHECK(interpolator.interpolateElapsed(startTime + std::chrono::seconds{10}) == duration);
    }

    SECTION("Unknown total does not erase the elapsed anchor or stop the playing clock")
    {
      auto const startTime = FrameClock::fromMicros(1'000'000);
      auto const elapsed = std::chrono::milliseconds{3200};
      interpolator.updateState(elapsed, {}, true);
      CHECK(interpolator.interpolateElapsed(startTime) == elapsed);
      CHECK(interpolator.interpolateElapsed(startTime + std::chrono::seconds{1}) == std::chrono::milliseconds{4200});
      interpolator.updateState(std::chrono::milliseconds{4035}, {}, false);
      CHECK(interpolator.interpolateElapsed(startTime + std::chrono::seconds{3}) == std::chrono::milliseconds{4035});
    }

    SECTION("An unknown total does not allow a large restored offset to wrap")
    {
      auto const startTime = FrameClock::fromMicros(0);
      auto const maximumElapsed = std::chrono::milliseconds::max();
      interpolator.updateState(maximumElapsed - std::chrono::milliseconds{1}, {}, true);
      REQUIRE(interpolator.interpolateElapsed(startTime) == maximumElapsed - std::chrono::milliseconds{1});
      CHECK(interpolator.interpolateElapsed(startTime + std::chrono::seconds{1}) == maximumElapsed);
    }

    SECTION("Frame clock moving backwards re-bases the interpolation")
    {
      interpolator.updateState(std::chrono::seconds{1}, std::chrono::seconds{5}, true);

      auto const startTime = FrameClock::fromMicros(2'000'000);
      CHECK(interpolator.interpolateElapsed(startTime) == std::chrono::seconds{1});

      // An earlier timestamp re-bases instead of producing a negative delta.
      auto const earlierTime = FrameClock::fromMicros(1'000'000);
      CHECK(interpolator.interpolateElapsed(earlierTime) == std::chrono::seconds{1});
      CHECK(interpolator.interpolateElapsed(earlierTime + std::chrono::milliseconds{250}) ==
            std::chrono::milliseconds{1250});
    }

    SECTION("Static value while paused")
    {
      interpolator.updateState(std::chrono::seconds{2}, std::chrono::seconds{5}, false);
      CHECK(interpolator.isPlaying() == false);
      CHECK(interpolator.interpolateElapsed(FrameClock::fromMicros(1'000'000)) == std::chrono::seconds{2});
      CHECK(interpolator.interpolateElapsed(FrameClock::fromMicros(2'000'000)) == std::chrono::seconds{2});
    }

    SECTION("Reset clears state")
    {
      interpolator.updateState(std::chrono::seconds{1}, std::chrono::seconds{5}, true);
      REQUIRE(interpolator.lastDuration() == std::chrono::seconds{5});
      REQUIRE(interpolator.isPlaying());
      interpolator.reset();
      CHECK(interpolator.lastDuration() == std::chrono::milliseconds{0});
      CHECK(interpolator.isPlaying() == false);
      CHECK(interpolator.interpolateElapsed(FrameClock::fromMicros(1'000'000)) == std::chrono::milliseconds{0});
    }
  }
} // namespace ao::uimodel::test
