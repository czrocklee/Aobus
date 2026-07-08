// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/audio/Property.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace ao::audio
{
  struct Format;

  struct RenderPcmResult final
  {
    std::size_t bytesWritten = 0;
    std::uint32_t positionFrameOffset = 0;
    std::uint32_t positionFrames = 0;
    bool drained = false;
  };

  /**
   * @brief Interface for receiving render requests and events from an audio backend.
   *
   * This interface decouples the Engine logic from the specific backend implementation.
   */
  class RenderTarget
  {
  public:
    virtual ~RenderTarget() = default;

    RenderTarget(RenderTarget const&) = delete;
    RenderTarget& operator=(RenderTarget const&) = delete;
    RenderTarget(RenderTarget&&) = delete;
    RenderTarget& operator=(RenderTarget&&) = delete;

    /**
     * @brief Called by the backend when it needs more PCM data.
     *
     * Frame-alignment contract: @p output.size() is always a whole multiple of
     * the negotiated frame size (bytesPerSample * channels), and
     * RenderPcmResult::bytesWritten MUST likewise be a whole multiple of the
     * frame size — a partial frame is never returned. Backends commit whole
     * frames to the device and treat any sub-frame remainder as undefined.
     * RenderPcmResult::positionFrameOffset is the rendered frame prefix that
     * belongs to the retired item before the current active item's frames
     * begin. RenderPcmResult::positionFrames is the frame count that should
     * advance the current active playback item after the backend commits the
     * full bytesWritten; it can be smaller than bytesWritten/frame when a
     * render crosses a gapless splice boundary. A short render is an underrun
     * unless RenderPcmResult::drained is true.
     */
    virtual RenderPcmResult renderPcm(std::span<std::byte> output) noexcept = 0;

    /// Called by the backend when an underrun occurs.
    virtual void onUnderrun() noexcept = 0;

    /// Called by the backend to report playback progress.
    virtual void onPositionAdvanced(std::uint32_t frames) noexcept = 0;

    /// Called by the backend when a drain operation has completed.
    virtual void onDrainComplete() noexcept = 0;

    /// Called by the backend when the stream's runtime node ID or route anchor is stable.
    virtual void onRouteReady(std::string_view routeAnchor) noexcept = 0;

    /// Called by the backend when its input stream format is negotiated or changes.
    virtual void onFormatChanged(Format const& format) noexcept = 0;

    /// Called by the backend when a runtime property changes externally.
    virtual void onPropertyChanged(PropertyId id) noexcept = 0;

    /// Called by the backend when a terminal error occurs (e.g. device lost).
    virtual void onBackendError(std::string_view message) noexcept = 0;

  protected:
    RenderTarget() = default;
  };
} // namespace ao::audio
