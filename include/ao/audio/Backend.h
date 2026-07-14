// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/Property.h>

#include <concepts>
#include <expected>

namespace ao::audio
{
  struct Format;
  class RenderTarget;

  /**
   * @brief Platform-specific audio output backend contract.
   *
   * Threading contract:
   * - Engine serializes application control commands before calling backend public
   *   methods, but backend callbacks may still be in flight while those methods
   *   run. Implementations must protect native handles against public method /
   *   callback interleavings.
   * - Backends may call RenderTarget methods from their render or backend event
   *   threads. They must not hold a native-handle lock while invoking a
   *   RenderTarget callback if a public backend method can acquire that same
   *   lock. Non-realtime Engine events are handed off internally, but the
   *   backend must still avoid callback/native-lock reentrancy hazards.
   * - stop() is called from the non-render Engine control domain. It closes
   *   render admission and does not return until every in-flight render cycle,
   *   including its RenderTarget render notifications, has returned. No new
   *   render cycle begins until start() reactivates the backend.
   *   Non-render route, property, and error callbacks remain governed by their
   *   existing generation checks and the close() lifetime boundary.
   * - close() is the render-target lifetime boundary. After close() returns, the
   *   backend must not issue further callbacks to the RenderTarget passed to
   *   open(), and all in-flight callbacks for that target must have returned.
   * - stop() stops active rendering but does not revoke the open target; seek-like
   *   flows may call stop(), flush(), and start() on the same target.
   */
  class Backend
  {
  public:
    virtual ~Backend() = default;

    Backend(Backend const&) = delete;
    Backend& operator=(Backend const&) = delete;
    Backend(Backend&&) = delete;
    Backend& operator=(Backend&&) = delete;

    virtual Result<> open(Format const& format, RenderTarget* target) = 0;

    virtual void start() = 0;
    virtual void pause() = 0;
    virtual void resume() = 0;
    virtual void flush() = 0;
    virtual void stop() = 0;
    virtual void close() = 0;

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

    virtual BackendId backendId() const = 0;
    virtual ProfileId profileId() const = 0;

  protected:
    Backend() = default;
  };
} // namespace ao::audio
