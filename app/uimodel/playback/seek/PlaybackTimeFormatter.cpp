// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/playback/seek/PlaybackPositionInteraction.h>

#include <chrono>
#include <format>
#include <string>

namespace ao::uimodel
{
  std::string describeTimeTemplate(PlaybackTimeMode mode)
  {
    switch (mode)
    {
      case PlaybackTimeMode::Elapsed:
      case PlaybackTimeMode::Duration: return "00:00";
      case PlaybackTimeMode::Combined:
      default: return "00:00 / 00:00";
    }
  }

  std::string formatPlaybackTime(PlaybackTimeMode mode,
                                 std::chrono::milliseconds elapsed,
                                 std::chrono::milliseconds duration)
  {
    auto const formatClock = [](std::chrono::milliseconds const time)
    {
      auto const seconds = std::chrono::duration_cast<std::chrono::seconds>(time).count();
      return std::format("{}:{:02d}", seconds / 60, seconds % 60);
    };

    if (mode == PlaybackTimeMode::Elapsed)
    {
      return formatClock(elapsed);
    }

    auto const total = duration > std::chrono::milliseconds::zero() ? formatClock(duration) : "--:--";
    return mode == PlaybackTimeMode::Duration ? total : std::format("{} / {}", formatClock(elapsed), total);
  }
} // namespace ao::uimodel
