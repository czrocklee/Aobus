// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/FrameClock.h>
#include <ao/uimodel/playback/seek/PlaybackPositionInterpolator.h>

#include <algorithm>
#include <chrono>

namespace ao::uimodel
{
  void PlaybackPositionInterpolator::updateState(std::chrono::milliseconds elapsed,
                                                 std::chrono::milliseconds duration,
                                                 bool isPlaying) noexcept
  {
    _lastElapsed = elapsed;
    _lastDuration = duration;
    _isPlaying = isPlaying;
    _optFirstFrameTime.reset();
  }

  void PlaybackPositionInterpolator::reset() noexcept
  {
    _lastElapsed = std::chrono::milliseconds{0};
    _lastDuration = std::chrono::milliseconds{0};
    _isPlaying = false;
    _optFirstFrameTime.reset();
  }

  std::chrono::milliseconds PlaybackPositionInterpolator::interpolateElapsed(FrameClock::TimePoint frameTime) noexcept
  {
    if (!_isPlaying)
    {
      _optFirstFrameTime.reset();
      return _lastElapsed;
    }

    if (!_optFirstFrameTime || frameTime < *_optFirstFrameTime)
    {
      _optFirstFrameTime = frameTime;
      return _lastElapsed;
    }

    auto const delta = std::chrono::duration_cast<std::chrono::milliseconds>(frameTime - *_optFirstFrameTime);
    auto const displayElapsed = _lastElapsed + delta;

    return std::min(displayElapsed, _lastDuration);
  }
} // namespace ao::uimodel
