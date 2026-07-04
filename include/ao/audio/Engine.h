// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/audio/AudioRouteFormatState.h>
#include <ao/audio/Backend.h>
#include <ao/audio/Format.h>
#include <ao/audio/PlaybackInput.h>
#include <ao/audio/Transport.h>

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
  struct PlaybackInput;
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
     * setOnTrackAdvanced(), and setOnRouteChanged() are delivered from Engine's
     * internal event worker, not from backend or decoder callback stacks.
     * setOnStateChanged() reports asynchronous backend/source state changes;
     * synchronous control commands publish their result by returning. Callbacks
     * may call back into Engine control methods. They must return promptly;
     * blocking a user callback blocks subsequent Engine event delivery.
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
      AudioRouteFormatState routeState;

      float volume = 1.0F;
      bool muted = false;
      bool volumeAvailable = false;
      bool volumeIsHardwareAssisted = false;

      bool operator==(Status const&) const = default;
    };

    struct RouteStatus final
    {
      AudioRouteFormatState state;
      std::optional<RouteAnchor> optAnchor;

      bool operator==(RouteStatus const&) const = default;
    };

    struct PlaybackItemId final
    {
      std::uint64_t value = 0;

      bool operator==(PlaybackItemId const&) const = default;
    };

    struct PlaybackItem final
    {
      PlaybackItemId id;
      PlaybackInput input;
    };

    enum class PreparedTransitionMode : std::uint8_t
    {
      Gapless,
      DrainFallback,
    };

    struct PreparedNextResult final
    {
      PlaybackItemId itemId;
      PreparedTransitionMode transition = PreparedTransitionMode::DrainFallback;
    };

    struct TrackAdvanced final
    {
      PlaybackItemId itemId;
      PlaybackInput input;
    };

    using OnRouteChanged = std::function<void(RouteStatus const&)>;
    using OnTrackAdvanced = std::function<void(TrackAdvanced const&)>;

    Engine(std::unique_ptr<IBackend> backendPtr, Device const& device, DecoderFactoryFn decoderFactory = nullptr);
    ~Engine();

    Engine(Engine const&) = delete;
    Engine& operator=(Engine const&) = delete;
    Engine(Engine&&) = delete;
    Engine& operator=(Engine&&) = delete;

    void setBackend(std::unique_ptr<IBackend> backendPtr, Device const& device);
    void updateDevice(Device const& device);

    void setOnTrackEnded(std::function<void()> callback);
    void setOnTrackAdvanced(OnTrackAdvanced callback);
    void setOnRouteChanged(OnRouteChanged callback);
    void setOnStateChanged(std::function<void()> callback);

    RouteStatus routeStatus() const;

    void play(PlaybackItem const& item);
    Result<PreparedNextResult> setNext(PlaybackItem const& item);
    std::optional<PlaybackItemId> clearNext();
    void pause();
    void resume();
    void stop();
    void seek(std::chrono::milliseconds offset);

    /// @brief Applies the volume to the backend, returning any device failure.
    /// The cached state always reflects the requested value regardless.
    Result<> setVolume(float volume);
    /// @brief Returns a possibly-torn live read of the volume.
    float volume() const;
    /// @brief Applies the mute state to the backend, returning any device failure.
    Result<> setMuted(bool muted);
    /// @brief Returns a possibly-torn live read of the mute state.
    bool isMuted() const;
    /// @brief Returns a possibly-torn live read of volume availability.
    bool isVolumeAvailable() const;

    /// @brief Returns an atomic consistent snapshot of the engine state.
    Status status() const;
    /// @brief Returns a possibly-torn live read of the transport state.
    Transport transport() const;
    /// @brief Returns a possibly-torn live read of the backend ID.
    BackendId backendId() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> _implPtr;
  };
} // namespace ao::audio
