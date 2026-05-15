// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/PlaybackPositionInterpolator.h"

#include <algorithm>
#include <cstdint>

namespace ao::gtk
{
  void PlaybackPositionInterpolator::updateState(std::uint32_t positionMs,
                                                 std::uint32_t durationMs,
                                                 bool isPlaying) noexcept
  {
    _lastPositionMs = positionMs;
    _lastDurationMs = durationMs;
    _isPlaying = isPlaying;
    _firstFrameTime = 0;
  }

  void PlaybackPositionInterpolator::reset() noexcept
  {
    _lastPositionMs = 0;
    _lastDurationMs = 0;
    _isPlaying = false;
    _firstFrameTime = 0;
  }

  std::uint32_t PlaybackPositionInterpolator::interpolate(std::int64_t frameTime) noexcept
  {
    if (!_isPlaying)
    {
      _firstFrameTime = 0;
      return _lastPositionMs;
    }

    if (_firstFrameTime == 0)
    {
      _firstFrameTime = frameTime;
    }

    // GDK frame time is in microseconds. Convert delta to milliseconds.
    constexpr double kMsScale = 1000.0;
    auto const elapsedMs = static_cast<std::uint32_t>(static_cast<double>(frameTime - _firstFrameTime) / kMsScale);
    auto const displayPos = _lastPositionMs + elapsedMs;

    return std::min(displayPos, _lastDurationMs);
  }
} // namespace ao::gtk
