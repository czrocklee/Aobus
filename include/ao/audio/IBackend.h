// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <ao/audio/Backend.h>

#include <ao/Error.h>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace ao::audio
{
  /**
   * @brief Callbacks provided by the engine to the backend.
   */
  struct RenderCallbacks final
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
    void (*onFormatChanged)(void* userData, Format const& format) noexcept = nullptr;

    /// Called by the backend when a terminal error occurs (e.g. device lost).
    void (*onBackendError)(void* userData, std::string_view message) noexcept = nullptr;
  };

  /**
   * @brief Interface for platform-specific audio output backends.
   */
  class IBackend
  {
  public:
    virtual ~IBackend() = default;

    /**
     * @brief Prepares the backend for playback with the given format.
     */
    virtual ao::Result<> open(Format const& format, RenderCallbacks callbacks) = 0;

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

    virtual BackendId backendId() const noexcept = 0;
    virtual ProfileId profileId() const noexcept = 0;
  };
} // namespace ao::audio
