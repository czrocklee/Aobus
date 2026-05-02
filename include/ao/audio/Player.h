// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <ao/audio/Backend.h>
#include <ao/audio/IBackendProvider.h>
#include <ao/audio/Types.h>
#include <ao/audio/flow/Graph.h>
#include <ao/utility/IMainThreadDispatcher.h>

#include <atomic>
#include <functional>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace ao::audio
{
  class Engine;
  struct Snapshot;
  struct EngineRouteSnapshot;

  /**
   * @brief High-level player that coordinates multiple backends and tracks playback state.
   * Manages audio routing graphs and quality analysis.
   */
  class Player final
  {
  public:
    explicit Player(std::shared_ptr<ao::IMainThreadDispatcher> dispatcher);
    ~Player();

    void addProvider(std::unique_ptr<IBackendProvider> provider);

    void play(TrackPlaybackDescriptor const& descriptor);
    void setOutput(BackendKind kind, std::string_view deviceId);
    void pause();
    void resume();
    void stop();
    void seek(std::uint32_t positionMs);

    [[nodiscard]] Snapshot snapshot() const;

    void setTrackEndedCallback(std::function<void()> callback);

    // Internal visibility for tests
    uint64_t _playbackGeneration = 1;
    void handleRouteChanged(EngineRouteSnapshot const& snapshot, std::uint64_t generation);

  private:
    void handleDevicesChanged(IBackendProvider* provider, std::vector<Device> const& devices);
    void handleSystemGraphChanged(flow::Graph const& graph, std::uint64_t generation);
    void updateMergedGraph();
    void analyzeAudioQuality();

    struct ProviderRecord
    {
      std::unique_ptr<IBackendProvider> provider;
      Subscription subscription;
      std::vector<Device> devices;
    };

    struct PendingOutput
    {
      BackendKind kind;
      std::string deviceId;
    };

    std::vector<std::unique_ptr<ProviderRecord>> _providers;
    std::optional<PendingOutput> _pendingOutput;
    IBackendProvider* _activeManager = nullptr;
    Subscription _graphSubscription;

    std::shared_ptr<ao::IMainThreadDispatcher> _dispatcher;
    std::unique_ptr<Engine> _engine;

    mutable std::vector<BackendSnapshot> _cachedBackends;
    mutable std::vector<Device> _allDevices;

    EngineRouteSnapshot _cachedEngineRoute;
    flow::Graph _cachedSystemGraph;
    flow::Graph _mergedGraph;

    Quality _quality = Quality::Unknown;
    std::string _qualityTooltip;

    std::function<void()> _onTrackEnded;

    // Quality analysis helpers
    std::vector<flow::Node const*> findPlaybackPath(std::string const& startId) const;
    void assessNodeQuality(flow::Node const& node, flow::Node const* nextNode);
    void processInputSources(flow::Node const& node,
                             std::span<flow::Node const* const> path,
                             std::unordered_map<std::string, std::set<std::string>> const& inputSources);
  };
} // namespace ao::audio
