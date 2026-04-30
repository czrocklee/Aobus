// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include "core/backend/BackendTypes.h"
#include "core/backend/IBackendManager.h"
#include "core/playback/PlaybackTypes.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <vector>

namespace app::core
{
  class IMainThreadDispatcher;
}

namespace app::core::backend
{
  class IBackendManager;
}

namespace app::core::playback
{
  class PlaybackEngine;

  class PlaybackController final
  {
  public:
    PlaybackController(std::shared_ptr<IMainThreadDispatcher> dispatcher);
    ~PlaybackController();

    void setTrackEndedCallback(std::function<void()> callback);

    void addManager(std::unique_ptr<backend::IBackendManager> manager);

    void play(TrackPlaybackDescriptor const& descriptor);

    void setOutput(backend::BackendKind kind, std::string_view deviceId);
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
    void handleSystemGraphChanged(backend::AudioGraph const& graph, std::uint64_t generation);

    std::unique_ptr<PlaybackEngine> _engine;

    // Backend managers
    std::vector<std::unique_ptr<backend::IBackendManager>> _managers;
    backend::IBackendManager* _activeManager = nullptr;

    std::shared_ptr<IMainThreadDispatcher> _dispatcher;
    std::function<void()> _onTrackEnded;

    mutable std::atomic<bool> _backendsDirty{true};
    mutable std::vector<BackendSnapshot> _cachedBackends;
    mutable std::vector<backend::AudioDevice> _allDevices;

    // Controller-owned state
    EngineRouteSnapshot _cachedEngineRoute;
    backend::AudioGraph _cachedSystemGraph;
    std::unique_ptr<backend::IGraphSubscription> _graphSubscription;

    backend::AudioGraph _mergedGraph;
    backend::AudioQuality _quality = backend::AudioQuality::Unknown;
    std::string _qualityTooltip;
  };

} // namespace app::core::playback
