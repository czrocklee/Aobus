// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/audio/DecoderTypes.h>
#include <ao/audio/Format.h>
#include <ao/audio/Property.h>
#include <ao/audio/Subscription.h>
#include <ao/audio/Types.h>
#include <ao/audio/detail/RouteTracker.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace ao::audio
{
  class IBackend;
  class ISource;
  class IDecoderSession;
  using DecoderFactoryFn = std::function<std::unique_ptr<IDecoderSession>(std::filesystem::path const&, Format)>;
}

namespace ao::audio
{
  class Engine final
  {
  public:
    struct Status final
    {
      Transport transport = Transport::Idle;
      BackendId backendId;
      ProfileId profileId;
      std::uint32_t positionMs = 0;
      std::uint32_t durationMs = 0;
      std::uint32_t bufferedMs = 0;
      std::uint32_t underrunCount = 0;
      std::string statusText;
      DeviceId currentDeviceId;
      detail::RouteState routeState;

      float volume = 1.0f;
      bool muted = false;
      bool volumeAvailable = false;

      bool operator==(Status const&) const = default;
    };

    struct RouteStatus final
    {
      detail::RouteState state;
      std::optional<RouteAnchor> optAnchor;

      bool operator==(RouteStatus const&) const = default;
    };

    using OnRouteChanged = std::function<void(RouteStatus const&)>;

    Engine(std::unique_ptr<IBackend> backend, Device const& device, DecoderFactoryFn decoderFactory = nullptr);
    ~Engine();

    Engine(Engine const&) = delete;
    Engine& operator=(Engine const&) = delete;
    Engine(Engine&&) = delete;
    Engine& operator=(Engine&&) = delete;

    void setBackend(std::unique_ptr<IBackend> backend, Device const& device);
    void updateDevice(Device const& device);

    [[nodiscard]] Subscription onTrackEnded(std::function<void()> callback);
    [[nodiscard]] Subscription onRouteChanged(OnRouteChanged callback);

    RouteStatus routeStatus() const;

    void play(TrackPlaybackDescriptor const& descriptor);
    void pause();
    void resume();
    void stop();
    void seek(std::uint32_t positionMs);

    void setVolume(float volume);
    float getVolume() const;
    void setMuted(bool muted);
    bool isMuted() const;
    bool isVolumeAvailable() const;

    Status status() const;
    Transport transport() const;
    BackendId backendId() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
  };
} // namespace ao::audio
