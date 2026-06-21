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

  /**
   * @brief Interface for receiving render requests and events from an audio backend.
   *
   * This interface decouples the Engine logic from the specific backend implementation.
   */
  class IRenderTarget
  {
  public:
    virtual ~IRenderTarget() = default;

    IRenderTarget(IRenderTarget const&) = delete;
    IRenderTarget& operator=(IRenderTarget const&) = delete;
    IRenderTarget(IRenderTarget&&) = delete;
    IRenderTarget& operator=(IRenderTarget&&) = delete;

    /**
     * @brief Called by the backend when it needs more PCM data.
     *
     * Frame-alignment contract: @p output.size() is always a whole multiple of
     * the negotiated frame size (bytesPerSample * channels), and the return
     * value MUST likewise be a whole multiple of the frame size — a partial
     * frame is never returned. Backends commit whole frames to the device and
     * treat any sub-frame remainder as undefined. A short read (return value <
     * output.size()) is the expected underrun/drain signal.
     */
    virtual std::size_t readPcm(std::span<std::byte> output) noexcept = 0;

    /// Called by the backend to check if the source has finished sending data.
    virtual bool isSourceDrained() noexcept = 0;

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
    IRenderTarget() = default;
  };
} // namespace ao::audio
