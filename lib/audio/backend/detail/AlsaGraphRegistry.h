// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/audio/OpenedPcmMode.h>
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
  /** @brief Operational mode for ALSA volume control. */
  enum class AlsaVolumeControlMode : std::uint8_t
  {
    Unavailable,
    HardwareMixer,
    SoftwareGain,
  };

  /** @brief Current runtime state for an ALSA route. */
  struct AlsaRouteState final
  {
    std::string routeAnchor;

    // The whole opened mode, not a copy of one format: the stream node shows
    // the client byte layout and the sink node shows the confirmed endpoint
    // signal, so the graph cannot disagree with what Engine validated.
    std::optional<OpenedPcmMode> optMode{};
    float volume = 1.0F;
    bool muted = false;
    AlsaVolumeControlMode volumeMode = AlsaVolumeControlMode::Unavailable;
  };

  struct AlsaGraphPublicationState;

  /** @brief Lifetime-safe backend handle for ALSA graph publication. */
  class AlsaGraphPublisher final
  {
  public:
    AlsaGraphPublisher() = default;

    void publish(AlsaRouteState state) const;
    void clear(std::string_view routeAnchor) const;

  private:
    explicit AlsaGraphPublisher(std::shared_ptr<AlsaGraphPublicationState> statePtr);

    std::shared_ptr<AlsaGraphPublicationState> _statePtr;

    friend class AlsaGraphRegistry;
  };

  /**
   * @brief Registry for ALSA graph snapshots.
   *
   * Providers own and retire this registry. Backends retain only a publisher
   * over the independently shared publication state, which becomes inert when
   * the registry shuts down.
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

    Subscription subscribe(std::string_view routeAnchor, Callback callback);
    AlsaGraphPublisher publisher() const;
    void publish(AlsaRouteState state) const;
    void clear(std::string_view routeAnchor) const;

    /** @brief Retires subscriptions and makes every publisher inert. */
    void shutdown() noexcept;

  private:
    std::shared_ptr<AlsaGraphPublicationState> _statePtr;
  };
} // namespace ao::audio::backend::detail
