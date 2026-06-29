// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/audio/Backend.h>
#include <ao/audio/Engine.h>
#include <ao/audio/IBackendProvider.h>
#include <ao/audio/QualityAnalyzer.h>
#include <ao/audio/Types.h>
#include <ao/audio/flow/Graph.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace ao::async
{
  class IExecutor;
}

namespace ao::audio
{
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
      std::vector<IBackendProvider::Status> availableBackends;
      flow::Graph flow;
      Quality quality = Quality::Unknown;
      std::vector<NodeQualityAssessment> qualityAssessments{};
      float volume = 1.0F;
      bool muted = false;
      bool volumeAvailable = false;
      bool volumeIsHardwareAssisted = false;
      bool isReady = false;

      bool operator==(Status const&) const = default;
    };

    explicit Player(async::IExecutor& executor);
    ~Player();

    Player(Player const&) = delete;
    Player& operator=(Player const&) = delete;
    Player(Player&&) = delete;
    Player& operator=(Player&&) = delete;

    void addProvider(std::unique_ptr<IBackendProvider> providerPtr);

    /// @brief Starts playback. Returns `InvalidState` when the backend is not
    /// yet ready (device discovery pending); the request is dropped in that case.
    Result<> play(PlaybackInput const& input);
    /// @brief Selects the output device. Returns `NotFound` when no provider is
    /// registered for `backend`. A not-yet-discovered device is stored as pending
    /// and reported as success.
    Result<> setOutputDevice(BackendId const& backend, DeviceId const& deviceId, ProfileId const& profile);
    void pause();
    void resume();
    void stop();
    void seek(std::chrono::milliseconds offset);

    Result<> setVolume(float vol);
    Result<> setMuted(bool muted);
    Result<> toggleMute();

    Status status() const;
    Transport transport() const;
    bool isReady() const;

    void setOnTrackEnded(std::function<void()> callback);
    void setOnStateChanged(std::function<void()> callback);

    /// Called when available output devices change; receives per-provider status snapshots.
    void setOnOutputDevicesChanged(std::function<void(std::vector<IBackendProvider::Status> const&)> callback);

    /// Called when playback quality or readiness changes.
    void setOnQualityChanged(std::function<void(Quality quality, bool ready)> callback);

    // Internal visibility for tests
    void handleRouteChanged(Engine::RouteStatus const& status, std::uint64_t generation);
    std::uint64_t playbackGeneration() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> _implPtr;
  };
} // namespace ao::audio
