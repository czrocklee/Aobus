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
#include <set>
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

    void addManager(std::unique_ptr<IBackendManager> manager);

    void play(TrackPlaybackDescriptor const& descriptor);

    void setOutput(BackendKind kind, std::string_view deviceId);
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
    void handleSystemGraphChanged(AudioGraph const& graph, std::uint64_t generation);

    std::vector<AudioNode const*> findPlaybackPath(std::string const& startId) const;
    void processInputSources(AudioNode const& node,
                             std::span<AudioNode const* const> path,
                             std::unordered_map<std::string, std::set<std::string>> const& inputSources);
    void assessNodeQuality(AudioNode const& node, AudioNode const* nextNode);

    std::unique_ptr<PlaybackEngine> _engine;

    // Backend managers
    std::vector<std::unique_ptr<IBackendManager>> _managers;
    IBackendManager* _activeManager = nullptr;

    std::shared_ptr<rs::IMainThreadDispatcher> _dispatcher;
    std::function<void()> _onTrackEnded;

    mutable std::atomic<bool> _backendsDirty{true};
    mutable std::vector<BackendSnapshot> _cachedBackends;
    mutable std::vector<AudioDevice> _allDevices;

    // Controller-owned state
    EngineRouteSnapshot _cachedEngineRoute;
    AudioGraph _cachedSystemGraph;
    std::unique_ptr<IGraphSubscription> _graphSubscription;

    AudioGraph _mergedGraph;
    AudioQuality _quality = AudioQuality::Unknown;
    std::string _qualityTooltip;
  };
} // namespace rs::audio
