// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <chrono>
#include <cstdint>
#include <string>

namespace ao::uimodel
{
  enum class PlaybackTimeMode : std::uint8_t
  {
    /// Elapsed and duration together, which is what a reading shows unless asked for less.
    Combined,
    Elapsed,
    Duration
  };

  // Widest text the mode can produce, for sizing a label so it does not resize
  // while the clock runs.
  std::string describeTimeTemplate(PlaybackTimeMode mode);

  std::string formatPlaybackTime(PlaybackTimeMode mode,
                                 std::chrono::milliseconds elapsed,
                                 std::chrono::milliseconds duration);
} // namespace ao::uimodel
