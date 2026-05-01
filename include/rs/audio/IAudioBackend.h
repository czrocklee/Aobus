// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <rs/audio/BackendTypes.h>

#include <cstddef>
#include <cstdint>
#include <rs/Error.h>
#include <span>
#include <string_view>

namespace rs::audio
{
  /**
   * @brief Callbacks provided by the engine to the backend.
   */
  struct AudioRenderCallbacks final
  {
    void* userData = nullptr;

    /// Called by the backend when it needs more PCM data.
    std::size_t (*readPcm)(void* userData, std::span<std::byte> output) noexcept = nullptr;

    /// Called by the backend to check if the source has finished sending data.
    bool (*isSourceDrained)(void* userData) noexcept = nullptr;

    /// Called by the backend when an underrun occurs.
    void (*onUnderrun)(void* userData) noexcept = nullptr;

    /// Called by the backend to report playback progress.
    void (*onPositionAdvanced)(void* userData, std::uint32_t frames) noexcept = nullptr;

    /// Called by the backend when a drain operation has completed.
    void (*onDrainComplete)(void* userData) noexcept = nullptr;

    /// Called by the backend when the stream's runtime node ID or route anchor is stable.
    void (*onRouteReady)(void* userData, std::string_view routeAnchor) noexcept = nullptr;

    /// Called by the backend when its input stream format is negotiated or changes.
    void (*onFormatChanged)(void* userData, AudioFormat const& format) noexcept = nullptr;

    /// Called by the backend when a terminal error occurs (e.g. device lost).
    void (*onBackendError)(void* userData, std::string_view message) noexcept = nullptr;
  };

  /**
   * @brief Interface for platform-specific audio output backends.
   */
  class IAudioBackend
  {
  public:
    virtual ~IAudioBackend() = default;

    /**
     * @brief Prepares the backend for playback with the given format.
     */
    virtual rs::Result<> open(AudioFormat const& format, AudioRenderCallbacks callbacks) = 0;

    /**
     * @brief Resets the backend, closing any open streams and detaching callbacks.
     */
    virtual void reset() = 0;

    virtual void start() = 0;
    virtual void pause() = 0;
    virtual void resume() = 0;
    virtual void flush() = 0;
    virtual void drain() = 0;
    virtual void stop() = 0;
    virtual void close() = 0;

    /**
     * @brief Enables or disables exclusive mode (for PipeWire backend).
     * In exclusive mode, the stream connects directly to the hardware node,
     * bypassing the mixer for bit-perfect output.
     */
    virtual void setExclusiveMode(bool exclusive) = 0;
    virtual bool isExclusiveMode() const noexcept = 0;

    virtual BackendKind kind() const noexcept = 0;
  };
} // namespace rs::audio
