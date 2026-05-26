// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <cstdint>

namespace ao::uimodel::playback
{
  class PlaybackPositionInterpolator final
  {
  public:
    void updateState(std::uint32_t positionMs, std::uint32_t durationMs, bool isPlaying) noexcept;

    void reset() noexcept;

    std::uint32_t interpolate(std::int64_t frameTime) noexcept;

    bool isPlaying() const noexcept { return _isPlaying; }
    std::uint32_t lastDurationMs() const noexcept { return _lastDurationMs; }

  private:
    std::uint32_t _lastPositionMs = 0;
    std::uint32_t _lastDurationMs = 0;
    bool _isPlaying = false;
    std::int64_t _firstFrameTime = 0;
  };
} // namespace ao::uimodel::playback
