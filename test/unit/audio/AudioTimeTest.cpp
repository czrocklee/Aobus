// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "lib/audio/AudioTime.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>

namespace ao::audio::test
{
  TEST_CASE("Audio time - clamps non-positive durations to sample zero", "[audio][unit][audio-time][regression]")
  {
    CHECK(durationToSamples(std::chrono::milliseconds::min(), 48000) == 0U);
    CHECK(durationToSamples(std::chrono::milliseconds{-1}, 48000) == 0U);
    CHECK(durationToSamples(std::chrono::milliseconds{0}, 48000) == 0U);
  }

  TEST_CASE("Audio time - converts positive durations to samples", "[audio][unit][audio-time]")
  {
    CHECK(durationToSamples(std::chrono::milliseconds{1}, 48000) == 48U);
    CHECK(durationToSamples(std::chrono::milliseconds{1000}, 44100) == 44100U);
  }
} // namespace ao::audio::test
