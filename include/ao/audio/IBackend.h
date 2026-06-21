// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/audio/Backend.h>
#include <ao/audio/Property.h>

#include <concepts>
#include <expected>

namespace ao::audio
{
  struct Format;
  class IRenderTarget;

  /**
   * @brief Interface for platform-specific audio output backends.
   *
   * Threading contract:
   * - Engine serializes application control commands before calling backend public
   *   methods, but backend callbacks may still be in flight while those methods
   *   run. Implementations must protect native handles against public method /
   *   callback interleavings.
   * - Backends may call IRenderTarget methods from their render or backend event
   *   threads. They must not hold a native-handle lock while invoking an
   *   IRenderTarget callback if a public backend method can acquire that same
   *   lock; callbacks may synchronously update Engine state.
   * - close() is the render-target lifetime boundary. After close() returns, the
   *   backend must not issue further callbacks to the IRenderTarget passed to
   *   open(), and all in-flight callbacks for that target must have returned.
   * - stop() stops active rendering but does not revoke the open target; seek-like
   *   flows may call stop(), flush(), and start() on the same target.
   */
  class IBackend
  {
  public:
    virtual ~IBackend() = default;

    IBackend(IBackend const&) = delete;
    IBackend& operator=(IBackend const&) = delete;
    IBackend(IBackend&&) = delete;
    IBackend& operator=(IBackend&&) = delete;

    /**
     * @brief Initialize and open the backend for playback.
     */
    virtual Result<> open(Format const& format, IRenderTarget* target) = 0;

    virtual void start() = 0;
    virtual void pause() = 0;
    virtual void resume() = 0;
    virtual void flush() = 0;
    virtual void stop() = 0;
    virtual void close() = 0;

    // Runtime control surface
    virtual Result<> setProperty(PropertyId id, PropertyValue const& value) = 0;
    virtual Result<PropertyValue> property(PropertyId id) const = 0;
    virtual PropertyInfo queryProperty(PropertyId id) const noexcept = 0;

    template<typename T, PropertyId Id>
      requires std::constructible_from<PropertyValue, T>
    Result<> set(TypedProperty<T, Id> /*tag*/, T value)
    {
      return setProperty(Id, PropertyValue{value});
    }

    template<typename T, PropertyId Id>
      requires std::constructible_from<PropertyValue, T>
    Result<T> get(TypedProperty<T, Id> /*tag*/) const
    {
      auto const result = property(Id);

      if (!result)
      {
        return std::unexpected{result.error()};
      }

      return std::get<T>(*result);
    }

    virtual BackendId backendId() const noexcept = 0;
    virtual ProfileId profileId() const noexcept = 0;

  protected:
    IBackend() = default;
  };
} // namespace ao::audio
