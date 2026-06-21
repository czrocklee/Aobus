// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/audio/Backend.h>
#include <ao/audio/Format.h>
#include <ao/audio/Types.h>
#include <ao/audio/detail/RouteTracker.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
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
    /**
     * @brief Thread-tolerant playback coordinator.
     *
     * Application control commands are serialized internally: concurrent calls
     * to play(), stop(), seek(), setBackend(), setVolume(), and setMuted() are
     * applied in one internal order. The order is an implementation detail and
     * does not encode user-intent priority.
     *
     * Query methods such as status(), routeStatus(), transport(), volume(), and
     * isMuted() are safe to call concurrently and return self-consistent
     * snapshots, but they are not linearized with in-flight control commands.
     */
    struct Status final
    {
      Transport transport = Transport::Idle;
      BackendId backendId;
      ProfileId profileId;
      std::chrono::milliseconds elapsed{0};
      std::chrono::milliseconds duration{0};
      std::chrono::milliseconds bufferedDuration{0};
      std::uint32_t underrunCount = 0;
      std::string statusText;
      DeviceId currentDeviceId;
      detail::RouteState routeState;

      float volume = 1.0F;
      bool muted = false;
      bool volumeAvailable = false;
      bool volumeIsHardwareAssisted = false;

      bool operator==(Status const&) const = default;
    };

    struct RouteStatus final
    {
      detail::RouteState state;
      std::optional<RouteAnchor> optAnchor;

      bool operator==(RouteStatus const&) const = default;
    };

    using OnRouteChanged = std::function<void(RouteStatus const&)>;

    Engine(std::unique_ptr<IBackend> backendPtr, Device const& device, DecoderFactoryFn decoderFactory = nullptr);
    ~Engine();

    Engine(Engine const&) = delete;
    Engine& operator=(Engine const&) = delete;
    Engine(Engine&&) = delete;
    Engine& operator=(Engine&&) = delete;

    void setBackend(std::unique_ptr<IBackend> backendPtr, Device const& device);
    void updateDevice(Device const& device);

    void setOnTrackEnded(std::function<void()> callback);
    void setOnRouteChanged(OnRouteChanged callback);

    RouteStatus routeStatus() const;

    void play(TrackPlaybackDescriptor const& descriptor);
    void pause();
    void resume();
    void stop();
    void seek(std::chrono::milliseconds offset);

    void setVolume(float volume);
    float volume() const;
    void setMuted(bool muted);
    bool isMuted() const;
    bool isVolumeAvailable() const;

    Status status() const;
    Transport transport() const;
    BackendId backendId() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> _implPtr;
  };
} // namespace ao::audio
