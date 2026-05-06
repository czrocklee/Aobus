// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/audio/Backend.h>
#include <ao/audio/Engine.h>
#include <ao/audio/IBackendProvider.h>
#include <ao/audio/Types.h>
#include <ao/audio/flow/Graph.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ao::audio
{
  /**
   * @brief High-level player that coordinates multiple backends and tracks playback state.
   * Manages audio routing graphs and quality analysis.
   */
  class Player final
  {
  public:
    struct Status final
    {
      Engine::Status engine;
      std::string trackTitle;
      std::string trackArtist;
      std::vector<IBackendProvider::Status> availableBackends;
      flow::Graph flow;
      Quality quality = Quality::Unknown;
      std::string qualityTooltip;
      float volume = 1.0f;
      bool muted = false;
      bool volumeAvailable = false;
      bool isReady = false;

      bool operator==(Status const&) const = default;
    };

    Player();
    ~Player();

    void addProvider(std::unique_ptr<IBackendProvider> provider);

    void play(TrackPlaybackDescriptor const& descriptor);
    void setOutput(BackendId const& backend, DeviceId const& deviceId, ProfileId const& profile);
    void pause();
    void resume();
    void stop();
    void seek(std::uint32_t positionMs);

    void setVolume(float vol);
    void setMuted(bool muted);
    void toggleMute();

    Status status() const;
    Transport transport() const;
    bool isReady() const;

    void setOnTrackEnded(std::function<void()> callback);

    /// Called when available output devices change; receives per-provider status snapshots.
    void setOnDevicesChanged(std::function<void(std::vector<IBackendProvider::Status> const&)> callback);

    /// Called when playback quality or readiness changes.
    void setOnQualityChanged(std::function<void(ao::audio::Quality quality, bool ready)> callback);

    // Internal visibility for tests
    void handleRouteChanged(Engine::RouteStatus const& status, std::uint64_t generation);
    std::uint64_t playbackGeneration() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
  };
} // namespace ao::audio
