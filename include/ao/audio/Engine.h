// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/audio/AudioRouteFormatState.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/Device.h>
#include <ao/audio/Format.h>
#include <ao/audio/PlaybackInput.h>
#include <ao/audio/RouteAnchor.h>
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
  class Backend;
  class PcmSource;
  class DecoderSession;
  struct PlaybackInput;
  using DecoderFactoryFn = std::function<std::unique_ptr<DecoderSession>(std::filesystem::path const&, Format)>;
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
     * setOnTrackAdvanced(), setOnPlaybackFailure(), and setOnRouteChanged() are
     * delivered from Engine's internal event worker, not from backend or decoder callback stacks.
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
      std::uint64_t generation = 0;

      bool operator==(RouteStatus const&) const = default;
    };

    struct PlaybackItemId final
    {
      std::uint64_t value = 0;

      bool operator==(PlaybackItemId const&) const = default;
    };

    struct PlaybackItem final
    {
      PlaybackItemId id = {};
      PlaybackInput input;
    };

    /**
     * @brief Monotonic proof that callbacks and prepared transitions from older
     * playback generations can no longer be published.
     *
     * A barrier covers generations strictly older than `generation`. The
     * accepted explicit start at `generation` remains live and is therefore not
     * covered by its own receipt.
     */
    struct PreparedCancellationBarrier final
    {
      std::uint64_t generation = 0;

      bool covers(std::uint64_t candidateGeneration) const noexcept { return candidateGeneration < generation; }

      bool operator==(PreparedCancellationBarrier const&) const = default;
    };

    /**
     * @brief Move-only, non-published explicit-play candidate.
     *
     * Staging opens and seeks the candidate source without changing the active
     * playback generation, current item, or prepared lookahead. Only
     * commitPlayback() may publish it.
     */
    class PreparedPlaybackStart final
    {
    public:
      ~PreparedPlaybackStart();

      PreparedPlaybackStart(PreparedPlaybackStart const&) = delete;
      PreparedPlaybackStart& operator=(PreparedPlaybackStart const&) = delete;
      PreparedPlaybackStart(PreparedPlaybackStart&&) noexcept;
      PreparedPlaybackStart& operator=(PreparedPlaybackStart&&) noexcept;

    private:
      struct Impl;

      explicit PreparedPlaybackStart(std::unique_ptr<Impl> implPtr);

      std::unique_ptr<Impl> _implPtr;

      friend class Engine;
    };

    struct PlaybackStartReceipt final
    {
      PlaybackItemId itemId;
      std::uint64_t generation = 0;
      PreparedCancellationBarrier cancellationBarrier;

      bool operator==(PlaybackStartReceipt const&) const = default;
    };

    enum class PreparedTransitionMode : std::uint8_t
    {
      Gapless,
      DrainFallback,
    };

    enum class PlaybackFailureKind : std::uint8_t
    {
      TrackOpen,
      Decode,
      RouteActivation,
      DeviceLost,
    };

    struct PreparedNextResult final
    {
      PlaybackItemId itemId;
      PreparedTransitionMode transition = PreparedTransitionMode::DrainFallback;
      std::uint64_t generation = 0;
    };

    struct TrackAdvanced final
    {
      PlaybackItemId itemId;
      PlaybackInput input;
      std::uint64_t generation = 0;
    };

    struct TrackEnded final
    {
      std::uint64_t generation = 0;
    };

    struct PlaybackFailure final
    {
      PlaybackFailureKind kind = PlaybackFailureKind::TrackOpen;
      PlaybackItemId itemId;
      PlaybackInput input;
      std::uint64_t generation = 0;
      Error error;
      bool recoverable = false;
    };

    using OnRouteChanged = std::function<void(RouteStatus const&)>;
    using OnTrackEnded = std::function<void(TrackEnded const&)>;
    using OnTrackAdvanced = std::function<void(TrackAdvanced const&)>;
    using OnPlaybackFailure = std::function<void(PlaybackFailure const&)>;

    Engine(std::unique_ptr<Backend> backendPtr, Device const& device, DecoderFactoryFn decoderFactory = nullptr);
    ~Engine();

    /// Stops worker threads and closes the backend while Engine remains addressable.
    /// Idempotent; no other methods may be called after shutdown.
    void shutdown() noexcept;

    Engine(Engine const&) = delete;
    Engine& operator=(Engine const&) = delete;
    Engine(Engine&&) = delete;
    Engine& operator=(Engine&&) = delete;

    void setBackend(std::unique_ptr<Backend> backendPtr, Device const& device);
    void updateDevice(Device const& device);

    void setOnTrackEnded(OnTrackEnded callback);
    void setOnTrackAdvanced(OnTrackAdvanced callback);
    void setOnPlaybackFailure(OnPlaybackFailure callback);
    void setOnRouteChanged(OnRouteChanged callback);
    void setOnStateChanged(std::function<void()> callback);

    /// Queues work on the event worker after any in-flight control command
    /// releases Engine's internal serialization lock.
    void defer(std::function<void()> callback);

    RouteStatus routeStatus() const;

    Result<PreparedPlaybackStart> stagePlayback(PlaybackItem const& item, std::chrono::milliseconds initialOffset = {});
    Result<PlaybackStartReceipt> commitPlayback(PreparedPlaybackStart&& preparedStart);
    PreparedCancellationBarrier stopWithBarrier();

    void play(PlaybackItem const& item, std::chrono::milliseconds initialOffset = {});
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
    /// @brief Returns the current audio callback/prepared-transition generation.
    std::uint64_t playbackGeneration() const noexcept;

  private:
    struct Impl;
    std::shared_ptr<Impl> _implPtr;
  };
} // namespace ao::audio
