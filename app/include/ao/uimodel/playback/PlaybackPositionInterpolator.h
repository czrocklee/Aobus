// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/uimodel/FrameClock.h>

#include <chrono>
#include <optional>

namespace ao::uimodel::playback
{
  class PlaybackPositionInterpolator final
  {
  public:
    void updateState(std::chrono::milliseconds elapsed, std::chrono::milliseconds duration, bool isPlaying) noexcept;

    void reset() noexcept;

    std::chrono::milliseconds interpolateElapsed(FrameClock::TimePoint frameTime) noexcept;

    bool isPlaying() const noexcept { return _isPlaying; }
    std::chrono::milliseconds lastDuration() const noexcept { return _lastDuration; }

  private:
    std::chrono::milliseconds _lastElapsed{0};
    std::chrono::milliseconds _lastDuration{0};
    bool _isPlaying = false;
    std::optional<FrameClock::TimePoint> _optFirstFrameTime;
  };
} // namespace ao::uimodel::playback
