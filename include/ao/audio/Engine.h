// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/audio/Backend.h>
#include <ao/audio/Format.h>
#include <ao/audio/Types.h>

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
     *
     * User callbacks registered through setOnStateChanged(), setOnTrackEnded(),
     * and setOnRouteChanged() are delivered from Engine's internal event worker,
     * not from backend or decoder callback stacks. setOnStateChanged() reports
     * asynchronous backend/source state changes; synchronous control commands
     * publish their result by returning. Callbacks may call back into Engine
     * control methods. They must return promptly; blocking a user callback
     * blocks subsequent Engine event delivery.
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
      RouteState routeState;

      float volume = 1.0F;
      bool muted = false;
      bool volumeAvailable = false;
      bool volumeIsHardwareAssisted = false;

      bool operator==(Status const&) const = default;
    };

    struct RouteStatus final
    {
      RouteState state;
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
    void setOnStateChanged(std::function<void()> callback);

    RouteStatus routeStatus() const;

    void play(PlaybackInput const& input);
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
