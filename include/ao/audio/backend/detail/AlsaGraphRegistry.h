// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/audio/Format.h>
#include <ao/audio/Subscription.h>
#include <ao/audio/flow/Graph.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace ao::audio::backend::detail
{
  /**
   * @brief Operational mode for ALSA volume control.
   */
  enum class AlsaVolumeControlMode : std::uint8_t
  {
    Unavailable,
    HardwareMixer,
    SoftwareGain,
  };

  /**
   * @brief Current runtime state for an ALSA route.
   */
  struct AlsaRouteState final
  {
    std::string routeAnchor;
    std::optional<Format> optFormat{};
    float volume = 1.0F;
    bool muted = false;
    AlsaVolumeControlMode volumeMode = AlsaVolumeControlMode::Unavailable;
  };

  /**
   * @brief Registry for ALSA graph snapshots.
   *
   * This class allows ALSA backends to publish their runtime state (volume, mute, mode)
   * and allows providers to serve live graph subscriptions to the Player.
   */
  class AlsaGraphRegistry final
  {
  public:
    using Callback = std::function<void(flow::Graph const&)>;

    AlsaGraphRegistry();
    ~AlsaGraphRegistry();

    AlsaGraphRegistry(AlsaGraphRegistry const&) = delete;
    AlsaGraphRegistry& operator=(AlsaGraphRegistry const&) = delete;
    AlsaGraphRegistry(AlsaGraphRegistry&&) = delete;
    AlsaGraphRegistry& operator=(AlsaGraphRegistry&&) = delete;

    /**
     * @brief Subscribes to graph updates for a specific route anchor.
     *
     * @param routeAnchor The unique identifier for the route (e.g. device name).
     * @param callback The function to invoke whenever the graph changes.
     * @return A Subscription that removes the callback on destruction.
     *
     * The returned subscription must be reset before this registry is
     * destroyed. A callback must not synchronously destroy the registry; defer
     * owner teardown until after publication returns.
     */
    Subscription subscribe(std::string_view routeAnchor, Callback callback);

    /**
     * @brief Publishes new state for an ALSA route.
     *
     * @param state The updated route state.
     */
    void publish(AlsaRouteState state);

    /**
     * @brief Clears the state for a route anchor, emitting an empty graph to subscribers.
     *
     * @param routeAnchor The identifier of the route to clear.
     */
    void clear(std::string_view routeAnchor);

  private:
    struct Impl;
    std::unique_ptr<Impl> _implPtr;
  };
} // namespace ao::audio::backend::detail
