// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/playback/PlaybackTimeViewModel.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>

namespace ao::gtk::test
{
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
      auto const elapsed = std::chrono::seconds{65}; // 1:05
      auto const duration = std::chrono::hours{1} + std::chrono::minutes{1} + std::chrono::seconds{1};

      CHECK(PlaybackTimeViewModel::formatPlaybackTime(PlaybackTimeMode::Elapsed, elapsed, duration) == "1:05");
      CHECK(PlaybackTimeViewModel::formatPlaybackTime(PlaybackTimeMode::Duration, elapsed, duration) == "61:01");
      CHECK(PlaybackTimeViewModel::formatPlaybackTime(PlaybackTimeMode::Default, elapsed, duration) == "1:05 / 61:01");
    }
  }
} // namespace ao::gtk::test
