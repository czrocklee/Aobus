// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/OpenedPcmMode.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/Property.h>
#include <ao/audio/SignalFormat.h>

#include <concepts>
#include <expected>
#include <optional>

namespace ao::audio
{
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
   * - All render notifications and the corresponding drain-complete callback
   *   form one serialized producer domain for an active target. They may run on
   *   different backend threads only when backend synchronization orders them
   *   without overlap. After renderPcm() reports drained, no new render cycle is
   *   admitted until start(), and at most one ordered drain-complete callback is
   *   emitted for that run.
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

    /**
     * @brief Returns a non-binding PCM format prediction for decoder prewarming.
     *
     * This method is non-blocking and performs no native open or negotiation.
     * Implementations may use immutable policy or cached observations. Engine
     * always treats open() as authoritative and discards a mismatched prewarm.
     */
    virtual std::optional<PcmFormat> prewarmFormatHint(SignalFormat const& sourceFormat) const noexcept;

    /**
     * @brief Opens one render target and returns the mode actually configured.
     *
     * `clientFormat` keeps the source sample rate and channel count and carries
     * the exact byte layout handed to the native API. It must preserve the
     * source precision. When a backend can confirm the physical endpoint, that
     * endpoint must preserve the source precision as well. Endpoint evidence is
     * route truth, not permission to reduce precision. A failed native open or
     * the absence of a lossless mode is reported as an error and is never
     * translated into an implicit lower-precision fallback.
     */
    virtual Result<OpenedPcmMode> open(SignalFormat const& sourceFormat, RenderTarget& target) = 0;

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
