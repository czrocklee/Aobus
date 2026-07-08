// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tui/PlaybackStatusFormatter.h"

#include <ao/audio/Transport.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>

namespace ao::tui::test
{
  TEST_CASE("PlaybackStatusFormatter - duration formatting is compact and stable", "[tui][unit][playback]")
  {
    CHECK(formatDuration(std::chrono::milliseconds{0}) == "0:00");
    CHECK(formatDuration(std::chrono::seconds{65}) == "1:05");
    CHECK(formatDuration(std::chrono::hours{1} + std::chrono::minutes{2} + std::chrono::seconds{3}) == "1:02:03");
    CHECK(formatDuration(-std::chrono::seconds{5}) == "0:00");
  }

  TEST_CASE("PlaybackStatusFormatter - transport labels describe playback state", "[tui][unit][playback]")
  {
    CHECK(transportLabel(audio::Transport::Idle) == "Idle");
    CHECK(transportLabel(audio::Transport::Opening) == "Opening");
    CHECK(transportLabel(audio::Transport::Buffering) == "Buffering");
    CHECK(transportLabel(audio::Transport::Playing) == "Playing");
    CHECK(transportLabel(audio::Transport::Paused) == "Paused");
    CHECK(transportLabel(audio::Transport::Seeking) == "Seeking");
    CHECK(transportLabel(audio::Transport::Stopping) == "Stopping");
    CHECK(transportLabel(audio::Transport::Error) == "Error");
  }

  TEST_CASE("PlaybackStatusFormatter - active playback transports need clock ticks", "[tui][unit][playback]")
  {
    CHECK_FALSE(needsTransportClockTick(audio::Transport::Idle));
    CHECK(needsTransportClockTick(audio::Transport::Opening));
    CHECK(needsTransportClockTick(audio::Transport::Buffering));
    CHECK(needsTransportClockTick(audio::Transport::Playing));
    CHECK_FALSE(needsTransportClockTick(audio::Transport::Paused));
    CHECK(needsTransportClockTick(audio::Transport::Seeking));
    CHECK_FALSE(needsTransportClockTick(audio::Transport::Stopping));
    CHECK_FALSE(needsTransportClockTick(audio::Transport::Error));
  }
} // namespace ao::tui::test
