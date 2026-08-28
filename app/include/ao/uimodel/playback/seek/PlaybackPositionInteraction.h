// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/uimodel/FrameClock.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace ao::uimodel
{
  /** Toolkit-neutral state and presentation rules for playback-position controls. */
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

  enum class PlaybackTimeMode : std::uint8_t
  {
    Combined,
    Elapsed,
    Duration
  };

  std::string describeTimeTemplate(PlaybackTimeMode mode);
  std::string formatPlaybackTime(PlaybackTimeMode mode,
                                 std::chrono::milliseconds elapsed,
                                 std::chrono::milliseconds duration);

  enum class SeekSliderAction : std::uint8_t
  {
    None,
    Preview,
    Commit,
  };

  struct SeekSliderUpdate final
  {
    SeekSliderAction action = SeekSliderAction::None;
    std::chrono::milliseconds elapsed{0};
  };

  class SeekInteraction final
  {
  public:
    void applyViewState(std::chrono::milliseconds duration, bool enabled) noexcept;
    void reset() noexcept;

    bool beginPointerInteraction() noexcept;
    SeekSliderUpdate endPointerInteraction(std::chrono::milliseconds elapsed) noexcept;
    SeekSliderUpdate valueChanged(std::chrono::milliseconds elapsed) noexcept;

    bool isPointerActive() const noexcept { return _pointerActive; }
    bool hasPendingFinalSeek() const noexcept { return _pendingFinalSeek; }
    std::chrono::milliseconds duration() const noexcept { return _duration; }

  private:
    std::chrono::milliseconds clampElapsed(std::chrono::milliseconds elapsed) const noexcept;

    std::chrono::milliseconds _duration{0};
    bool _enabled = false;
    bool _pointerActive = false;
    bool _pendingFinalSeek = false;
  };
} // namespace ao::uimodel
