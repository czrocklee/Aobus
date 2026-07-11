// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/BackendProvider.h>
#include <ao/audio/Device.h>
#include <ao/audio/Engine.h>
#include <ao/audio/Quality.h>
#include <ao/audio/QualityAnalyzer.h>
#include <ao/audio/Transport.h>
#include <ao/audio/flow/Graph.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace ao::async
{
  class Executor;
}

namespace ao::audio
{
  struct PlaybackInput;

  /**
   * @brief High-level player that coordinates multiple backends and tracks playback state.
   * Manages audio routing graphs and quality analysis.
   *
   * State-change callbacks are delivered after asynchronous Engine
   * backend/source events, provider device/graph events, or Player
   * output-selection readiness changes. Player marshals those internal
   * reactions onto the executor passed at construction before touching
   * executor-owned Player state, so user callbacks always run on the
   * executor's owning thread.
   *
   * Threading contract: public methods and the destructor are expected to run on
   * the executor's owning thread, mirroring the layers that aggregate Player
   * (e.g. ao::rt::PlaybackService). The executor must outlive Player. Teardown
   * neutralizes marshalled callbacks that are still queued when destruction
   * starts; callbacks already running are on the executor thread and must return
   * promptly.
   */
  class Player final
  {
  public:
    struct Status final
    {
      Engine::Status engine;
      std::vector<BackendProvider::Status> availableBackends;
      flow::Graph flow;
      Quality sourceQuality = Quality::Unknown;
      Quality pipelineQuality = Quality::Unknown;
      Quality quality = Quality::Unknown;
      bool qualityFullyVerified = true;
      std::vector<NodeQualityAssessment> qualityAssessments{};
      float volume = 1.0F;
      bool muted = false;
      bool volumeAvailable = false;
      bool volumeIsHardwareAssisted = false;
      bool isReady = false;

      bool operator==(Status const&) const = default;
    };

    explicit Player(async::Executor& executor);
    Player(async::Executor& executor, DecoderFactoryFn decoderFactory);
    ~Player();

    /// Stops providers and quiesces Engine callbacks before an owner releases Player.
    /// Idempotent; no other methods may be called after shutdown.
    void shutdown() noexcept;

    Player(Player const&) = delete;
    Player& operator=(Player const&) = delete;
    Player(Player&&) = delete;
    Player& operator=(Player&&) = delete;

    void addProvider(std::unique_ptr<BackendProvider> providerPtr);

    /// @brief Starts playback. Returns `InvalidState` when the backend is not
    /// yet ready (device discovery pending); the request is dropped in that case.
    Result<> play(Engine::PlaybackItem const& item, std::chrono::milliseconds initialOffset = {});
    Result<Engine::PreparedPlaybackStart> stagePlayback(Engine::PlaybackItem const& item,
                                                        std::chrono::milliseconds initialOffset = {});
    Result<Engine::PlaybackStartReceipt> commitPlayback(Engine::PreparedPlaybackStart&& preparedStart);
    Result<Engine::PreparedNextResult> prepareNext(Engine::PlaybackItem const& item);
    std::optional<Engine::PlaybackItemId> clearPreparedNext();
    /// @brief Selects the output device. Returns `NotFound` when no provider is
    /// registered for `backend`. A not-yet-discovered device is stored as pending
    /// and reported as success.
    Result<> setOutputDevice(BackendId const& backend, DeviceId const& deviceId, ProfileId const& profile);
    void pause();
    void resume();
    Engine::PreparedCancellationBarrier stopWithBarrier();
    void stop();
    void seek(std::chrono::milliseconds offset);

    Result<> setVolume(float vol);
    Result<> setMuted(bool muted);
    Result<> toggleMute();

    Status status() const;
    Transport transport() const;
    bool isReady() const;

    void setOnTrackEnded(std::function<void(Engine::TrackEnded const&)> callback);
    void setOnTrackAdvanced(std::function<void(Engine::TrackAdvanced const&)> callback);
    void setOnPlaybackFailure(std::function<void(Engine::PlaybackFailure const&)> callback);
    void setOnStateChanged(std::function<void()> callback);

    /// Called when available output devices change; receives per-provider status snapshots.
    void setOnOutputDevicesChanged(std::function<void(std::vector<BackendProvider::Status> const&)> callback);

    /// Called when playback quality or readiness changes.
    void setOnQualityChanged(std::function<void(QualityResult const& quality, bool ready)> callback);

    // Internal visibility for tests
    void handleRouteChanged(Engine::RouteStatus const& status, std::uint64_t generation);
    std::uint64_t playbackGeneration() const noexcept;
    std::uint64_t audioPlaybackGeneration() const noexcept;

  private:
    struct Impl;
    std::shared_ptr<Impl> _implPtr;
  };
} // namespace ao::audio
