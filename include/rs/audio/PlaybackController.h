// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <rs/audio/BackendTypes.h>
#include <rs/audio/IBackendManager.h>
#include <rs/audio/PlaybackTypes.h>
#include <rs/utility/IMainThreadDispatcher.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <vector>

namespace rs::audio
{
  class IBackendManager;
  class PlaybackEngine;

  class PlaybackController final
  {
  public:
    PlaybackController(std::shared_ptr<rs::IMainThreadDispatcher> dispatcher);
    ~PlaybackController();

    void setTrackEndedCallback(std::function<void()> callback);

    void addManager(std::unique_ptr<rs::audio::IBackendManager> manager);

    void play(TrackPlaybackDescriptor const& descriptor);

    void setOutput(rs::audio::BackendKind kind, std::string_view deviceId);
    void pause();
    void resume();
    void stop();
    void seek(std::uint32_t positionMs);

    PlaybackSnapshot snapshot() const;

    // Public for testing
    void handleRouteChanged(EngineRouteSnapshot const& snapshot, std::uint64_t generation);
    void updateMergedGraph();
    void analyzeAudioQuality();

    std::uint64_t _playbackGeneration = 0;

  private:
    void handleSystemGraphChanged(rs::audio::AudioGraph const& graph, std::uint64_t generation);

    std::unique_ptr<PlaybackEngine> _engine;

    // Backend managers
    std::vector<std::unique_ptr<rs::audio::IBackendManager>> _managers;
    rs::audio::IBackendManager* _activeManager = nullptr;

    std::shared_ptr<rs::IMainThreadDispatcher> _dispatcher;
    std::function<void()> _onTrackEnded;

    mutable std::atomic<bool> _backendsDirty{true};
    mutable std::vector<BackendSnapshot> _cachedBackends;
    mutable std::vector<rs::audio::AudioDevice> _allDevices;

    // Controller-owned state
    EngineRouteSnapshot _cachedEngineRoute;
    rs::audio::AudioGraph _cachedSystemGraph;
    std::unique_ptr<rs::audio::IGraphSubscription> _graphSubscription;

    rs::audio::AudioGraph _mergedGraph;
    rs::audio::AudioQuality _quality = rs::audio::AudioQuality::Unknown;
    std::string _qualityTooltip;
  };

} // namespace rs::audio
