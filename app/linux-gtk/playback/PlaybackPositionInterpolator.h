// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <algorithm>
#include <cstdint>

namespace ao::gtk
{
  /**
   * @brief Utility for interpolating playback position between state updates using the frame clock.
   *
   * This class centralizes the interpolation logic previously duplicated in multiple UI components.
   * It calculates an estimated display position based on the time elapsed since the last
   * position update from the audio engine.
   */
  class PlaybackPositionInterpolator final
  {
  public:
    /**
     * @brief Updates the internal state with new data from the playback service.
     * @param positionMs The current position in milliseconds.
     * @param durationMs The total duration in milliseconds.
     * @param isPlaying Whether playback is currently active.
     */
    void updateState(std::uint32_t positionMs, std::uint32_t durationMs, bool isPlaying) noexcept;

    /**
     * @brief Resets the interpolator to its initial idle state.
     */
    void reset() noexcept;

    /**
     * @brief Interpolates the playback position for the current frame.
     * @param frameTime The current time from the GDK frame clock (in microseconds).
     * @return The interpolated position in milliseconds, capped at duration.
     */
    [[nodiscard]] std::uint32_t interpolate(std::int64_t frameTime) noexcept;

    [[nodiscard]] bool isPlaying() const noexcept { return _isPlaying; }
    [[nodiscard]] std::uint32_t lastDurationMs() const noexcept { return _lastDurationMs; }

  private:
    std::uint32_t _lastPositionMs = 0;
    std::uint32_t _lastDurationMs = 0;
    bool _isPlaying = false;
    std::int64_t _firstFrameTime = 0;
  };
} // namespace ao::gtk
