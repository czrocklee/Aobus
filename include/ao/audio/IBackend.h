// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <ao/audio/Backend.h>
#include <ao/audio/Property.h>

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

    /// Called by the backend when a runtime property changes externally.
    void (*onPropertyChanged)(void* userData, PropertyId id) noexcept = nullptr;

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

    virtual void start() = 0;
    virtual void pause() = 0;
    virtual void resume() = 0;
    virtual void flush() = 0;
    virtual void stop() = 0;
    virtual void close() = 0;

    // Runtime control surface
    virtual Result<> setProperty(PropertyId id, PropertyValue const& value) = 0;
    virtual Result<PropertyValue> getProperty(PropertyId id) const = 0;
    virtual PropertyInfo queryProperty(PropertyId id) const noexcept = 0;

    template<typename T, PropertyId Id>
    Result<> set(TypedProperty<T, Id>, T value)
    {
      return setProperty(Id, PropertyValue{value});
    }

    template<typename T, PropertyId Id>
    Result<T> get(TypedProperty<T, Id>) const
    {
      auto const result = getProperty(Id);

      if (!result)
      {
        return std::unexpected(result.error());
      }

      return std::get<T>(*result);
    }

    virtual BackendId backendId() const noexcept = 0;
    virtual ProfileId profileId() const noexcept = 0;
  };
} // namespace ao::audio
