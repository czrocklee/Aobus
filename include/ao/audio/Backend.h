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
#include <cstdint>
#include <expected>
#include <optional>

namespace ao::audio
{
  class RenderTarget;

  /** @brief Original open error plus the backend's causal admission decision. */
  struct BackendOpenFailure final
  {
    enum class Cause : std::uint8_t
    {
      OutputFailure,
      TrackRouteIncompatible,
    };

    Error error{};
    Cause cause = Cause::OutputFailure;
  };

  using BackendOpenResult = std::expected<OpenedPcmMode, BackendOpenFailure>;

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
   *   Render-domain callbacks must not synchronously invoke backend controls.
   * - Engine calls stop() from its non-render control domain. It closes
   *   render admission and does not return until every in-flight render cycle,
   *   including its RenderTarget render notifications, has returned. No new
   *   render cycle begins until start() reactivates the backend. Core Audio
   *   accepts run-only start()/stop() from its route/format notification
   *   origins under the graph-delivery contract in the audio execution
   *   documentation; this does not imply the same reentry support for other
   *   backends.
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
   *   A RenderTarget callback must defer close() and target replacement, including
   *   open(), on its invoking backend until it returns. This also covers nested
   *   calls on that stack; there is no self-callback completion exception.
   * - Core Audio target callbacks must defer operations that may wait for their
   *   shared graph delivery gate until the callback returns, including indirect
   *   operations on another backend sharing that registry. They must not wait
   *   for the deferred work. Non-render callbacks may prepare graph facts without
   *   delivering them; the audio execution graph-delivery contract defines the
   *   gate boundary. This is a caller precondition, not a runtime rejection.
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
     * Implementations may use immutable policy or cached observations, but must
     * withhold a hint for locally known admission refusals. Engine always treats
     * open() as authoritative and discards a mismatched prewarm.
     */
    virtual std::optional<PcmFormat> prewarmFormatHint(SignalFormat const& sourceFormat) const noexcept;

    /**
     * @brief Checks whether an installed transport can carry another canonical mode.
     *
     * The default requires exact PCM identity. An override must retain rate,
     * channel count, and concrete encoding; it may accept a different mask only
     * when native byte ordering and every permitted layout refresh are
     * equivalent for both modes. It must not infer source identity or change the
     * installed native configuration. Retained mapping evidence may remain
     * conservative; this query does not manufacture a new native observation.
     *
     * Engine calls this under control serialization, without its state or
     * transition locks, and repeats it when adopting prepared playback. It must
     * perform no native I/O or outward callbacks. Success is not a liveness lease
     * and does not waive source or confirmed-endpoint precision validation.
     */
    virtual bool canReuseFormat(PcmFormat const& currentFormat, PcmFormat const& nextFormat) const noexcept;

    /**
     * @brief Opens one render target and returns the mode actually configured.
     *
     * `clientFormat` keeps the source rate, count and effective identity in
     * canonical render-target order. Native slot ordering is backend-owned and
     * may use a lossless permutation. It must preserve source precision.
     * When a backend can confirm the physical endpoint, that
     * endpoint must preserve the source precision as well. Endpoint evidence is
     * route truth, not permission to reduce precision. A failed native open or
     * the absence of a lossless mode is reported as an error and is never
     * translated into an implicit lower-precision fallback.
     *
     * TrackRouteIncompatible identifies a source or selected-route constraint
     * that another track may satisfy. OutputFailure is the conservative default
     * for native faults and uncertain causes. Neither the Error code nor its
     * message determines this distinction. The original Error is preserved.
     * Engine closes its previous stream before this call; an implementation's
     * early admission check does not promise to preserve Engine playback.
     *
     * A successful result is evidence of the mode actually configured on the
     * native handle for this attempt, not a liveness lease: a permitted
     * observer-driven close or target retirement may already have completed, or
     * may complete before open() returns. The result is never a mere format
     * prediction. A caller exposing raw backend observers must itself coordinate
     * the observer controls it allows; this contract does not authorize
     * reentrant Engine controls.
     */
    virtual BackendOpenResult open(SignalFormat const& sourceFormat, RenderTarget& target) = 0;

    virtual void start() = 0;
    virtual void pause() = 0;
    virtual void resume() = 0;
    virtual void flush() = 0;
    virtual void stop() = 0;
    virtual void close() = 0;

    virtual Result<> setProperty(PropertyId id, PropertyValue const& value) = 0;

    /**
     * @brief Observes one property value.
     *
     * A concrete backend may refresh native state and synchronously publish the
     * corresponding provider graph before returning. Callers must not hold a
     * state lock that graph subscribers may re-enter.
     */
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
      auto const res = property(Id);

      if (!res)
      {
        return std::unexpected{res.error()};
      }

      return std::get<T>(*res);
    }

    virtual BackendId backendId() const = 0;
    virtual ProfileId profileId() const = 0;

  protected:
    Backend() = default;
  };
} // namespace ao::audio
