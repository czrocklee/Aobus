// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/audio/Format.h>
#include <ao/audio/Subscription.h>
#include <ao/audio/flow/Graph.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace ao::audio::backend::detail
{
  /**
   * @brief Current runtime state for a WASAPI route.
   *
   * Volume and mute reflect the per-application session controls
   * (ISimpleAudioVolume); the Windows audio engine applies them downstream of
   * the stream, so they are classified as software gain. The input format is
   * the PCM contract passed to IAudioClient, while the mix format is queried
   * independently from the endpoint and remains unknown when Windows does not
   * expose a supported PCM representation.
   */
  struct WasapiRouteState final
  {
    std::string routeAnchor;
    std::optional<Format> optInputFormat{};
    std::optional<Format> optMixFormat{};
    float volume = 1.0F;
    bool muted = false;
  };

  /**
   * @brief Registry for WASAPI graph snapshots.
   *
   * Allows WASAPI backends to publish their runtime state (volume, mute,
   * format) and the provider to serve live graph subscriptions to the Player.
   */
  class WasapiGraphRegistry final
  {
  public:
    using Callback = std::function<void(flow::Graph const&)>;

    WasapiGraphRegistry();
    ~WasapiGraphRegistry();

    WasapiGraphRegistry(WasapiGraphRegistry const&) = delete;
    WasapiGraphRegistry& operator=(WasapiGraphRegistry const&) = delete;
    WasapiGraphRegistry(WasapiGraphRegistry&&) = delete;
    WasapiGraphRegistry& operator=(WasapiGraphRegistry&&) = delete;

    /**
     * @brief Subscribes to graph updates for a specific route anchor.
     *
     * @param routeAnchor The unique identifier for the route (endpoint ID).
     * @param callback The function to invoke whenever the graph changes.
     * @return A Subscription that removes the callback on destruction.
     *
     * The returned subscription must be reset before this registry is
     * destroyed. A callback must not synchronously destroy the registry; defer
     * owner teardown until after publication returns.
     */
    Subscription subscribe(std::string_view routeAnchor, Callback callback);

    /**
     * @brief Publishes new state for a WASAPI route.
     */
    void publish(WasapiRouteState state);

    /**
     * @brief Clears the state for a route anchor, emitting an empty graph to subscribers.
     */
    void clear(std::string_view routeAnchor);

    /**
     * @brief Clears all routes and permanently disables further publications.
     */
    void shutdown() noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> _implPtr;
  };
} // namespace ao::audio::backend::detail
