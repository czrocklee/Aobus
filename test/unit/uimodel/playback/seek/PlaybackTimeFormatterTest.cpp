// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/playback/seek/PlaybackTimeFormatter.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>

namespace ao::uimodel::test
{
  TEST_CASE("PlaybackTimeFormatter - formats display text for each label mode", "[uimodel][unit][playback]")
  {
    SECTION("template text reserves the widest idle label")
    {
      CHECK(describeTimeTemplate(PlaybackTimeMode::Elapsed) == "00:00");
      CHECK(describeTimeTemplate(PlaybackTimeMode::Duration) == "00:00");
      CHECK(describeTimeTemplate(PlaybackTimeMode::Default) == "00:00 / 00:00");
    }

    SECTION("playback time text matches selected label mode")
    {
      auto const elapsed = std::chrono::seconds{65};
      auto const duration = std::chrono::hours{1} + std::chrono::minutes{1} + std::chrono::seconds{1};

      CHECK(formatPlaybackTime(PlaybackTimeMode::Elapsed, elapsed, duration) == "1:05");
      CHECK(formatPlaybackTime(PlaybackTimeMode::Duration, elapsed, duration) == "61:01");
      CHECK(formatPlaybackTime(PlaybackTimeMode::Default, elapsed, duration) == "1:05 / 61:01");
    }
  }
} // namespace ao::uimodel::test
