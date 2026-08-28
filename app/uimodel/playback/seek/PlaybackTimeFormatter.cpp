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
    auto const elapsedSec = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();

    switch (auto const durSec = std::chrono::duration_cast<std::chrono::seconds>(duration).count(); mode)
    {
      case PlaybackTimeMode::Elapsed: return std::format("{:d}:{:02d}", elapsedSec / 60, elapsedSec % 60);

      case PlaybackTimeMode::Duration: return std::format("{:d}:{:02d}", durSec / 60, durSec % 60);

      case PlaybackTimeMode::Combined:
      default:
        return std::format("{:d}:{:02d} / {:d}:{:02d}", elapsedSec / 60, elapsedSec % 60, durSec / 60, durSec % 60);
    }
  }
} // namespace ao::uimodel
