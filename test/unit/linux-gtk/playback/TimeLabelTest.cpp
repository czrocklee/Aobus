// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/playback/PlaybackTimeViewModel.h>
#include <cstdint>

#include <catch2/catch_test_macros.hpp>

using namespace ao::uimodel::playback;

TEST_CASE("PlaybackTimeViewModel - string templates and formatting", "[unit][viewmodel][playback]")
{
  SECTION("describeTimeTemplate")
  {
    CHECK(PlaybackTimeViewModel::describeTimeTemplate(PlaybackTimeMode::Elapsed) == "00:00");
    CHECK(PlaybackTimeViewModel::describeTimeTemplate(PlaybackTimeMode::Duration) == "00:00");
    CHECK(PlaybackTimeViewModel::describeTimeTemplate(PlaybackTimeMode::Default) == "00:00 / 00:00");
  }

  SECTION("formatPlaybackTime")
  {
    std::uint32_t const posMs = 65000;   // 1:05
    std::uint32_t const durMs = 3661000; // 61:01 (1h 1m 1s)

    CHECK(PlaybackTimeViewModel::formatPlaybackTime(PlaybackTimeMode::Elapsed, posMs, durMs) == "1:05");
    CHECK(PlaybackTimeViewModel::formatPlaybackTime(PlaybackTimeMode::Duration, posMs, durMs) == "61:01");
    CHECK(PlaybackTimeViewModel::formatPlaybackTime(PlaybackTimeMode::Default, posMs, durMs) == "1:05 / 61:01");
  }
}
