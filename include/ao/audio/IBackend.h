// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/audio/Backend.h>
#include <ao/audio/IRenderTarget.h>
#include <ao/audio/Property.h>

#include <ao/Error.h>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace ao::audio
{
  /**
   * @brief Interface for platform-specific audio output backends.
   */
  class IBackend
  {
  public:
    virtual ~IBackend() = default;

    /**
     * @brief Initialize and open the backend for playback.
     */
    virtual ao::Result<> open(Format const& format, IRenderTarget* target) = 0;

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
