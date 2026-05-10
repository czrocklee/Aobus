// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/audio/Engine.h>
#include <ao/audio/IBackendProvider.h>
#include <ao/audio/NullBackend.h>
#include <ao/audio/Player.h>
#include <ao/audio/QualityAnalyzer.h>
#include <ao/utility/Log.h>

#include <algorithm>
#include <atomic>
#include <format>
#include <map>
#include <mutex>
#include <ranges>
#include <set>
#include <unordered_map>

namespace ao::audio
{
  namespace
  {
  }

  struct Player::Impl final
  {
    struct ProviderRecord
    {
      std::unique_ptr<IBackendProvider> provider;
      Subscription subscription;
      std::vector<Device> devices;
    };

    struct PendingOutput
    {
      BackendId backend;
      DeviceId deviceId;
      ProfileId profile;
    };

    Impl() = default;

    Impl(Impl const&) = delete;
    Impl(Impl&&) = delete;
    auto operator=(Impl const&) -> Impl& = delete;
    auto operator=(Impl&&) -> Impl& = delete;

    ~Impl()
    {
      // Order matters: stop PipeWire threads before their callback targets are destroyed.
      graphSubscription.reset(); // 1. remove subscription from PipeWireMonitor (still alive)
      engine.reset();            // 2. stop PipeWire playback thread
      providers.clear();         // 3. stop PipeWire monitor thread
    }

    std::uint64_t playbackGeneration = 1;
    std::vector<std::unique_ptr<ProviderRecord>> providers;
    std::optional<PendingOutput> pendingOutput;
    IBackendProvider* activeManager = nullptr;
    Subscription graphSubscription;
    std::unique_ptr<Engine> engine;

    mutable std::mutex backendsMutex;
    mutable std::vector<IBackendProvider::Status> cachedBackends;
    mutable std::vector<Device> allDevices;

    Engine::RouteStatus cachedRouteStatus;
    flow::Graph cachedSystemGraph;
    flow::Graph mergedGraph;

    QualityResult qualityResult;
    std::optional<TrackPlaybackDescriptor> currentTrack;

    std::function<void()> onTrackEnded;
    std::function<void(std::vector<IBackendProvider::Status> const&)> onDevicesChanged;
    std::function<void(ao::audio::Quality, bool)> onQualityChanged;

    void handleDevicesChanged(Player* owner, IBackendProvider* provider, std::vector<Device> const& devices);
    void handleSystemGraphChanged(Player* owner, flow::Graph const& graph, std::uint64_t generation);
    void updateMergedGraph();
  };

  void Player::Impl::handleDevicesChanged(Player* owner, IBackendProvider* provider, std::vector<Device> const& devices)
  {
    // Update individual provider cache
    auto const it =
      std::ranges::find_if(providers, [&](auto const& record) { return record->provider.get() == provider; });
    if (it != providers.end())
    {
      (*it)->devices = devices;
    }

    // Rebuild global cache from all providers
    auto allDevicesList = std::vector<Device>{};
    auto snapshots = std::vector<IBackendProvider::Status>{};

    for (auto const& record : providers)
    {
      auto status = record->provider->status();
      // The provider status might have its own internal device list, but we use the one from the subscription for
      // consistency
      status.devices = record->devices;
      snapshots.push_back(std::move(status));
      allDevicesList.insert(allDevicesList.end(), record->devices.begin(), record->devices.end());
    }

    {
      std::lock_guard<std::mutex> lock(backendsMutex);
      cachedBackends = std::move(snapshots);
      allDevices = std::move(allDevicesList);
    }

    // Keep the engine's current device capabilities up-to-date
    auto const currentSnap = engine->status();
    auto allDevicesCopy = std::vector<Device>{};
    {
      std::lock_guard<std::mutex> lock(backendsMutex);
      allDevicesCopy = allDevices;
    }
    auto const activeIt =
      std::ranges::find_if(allDevicesCopy,
                           [&](Device const& dev)
                           { return dev.backendId == currentSnap.backendId && dev.id == currentSnap.currentDeviceId; });

    if (activeIt != allDevicesCopy.end())
    {
      engine->updateDevice(*activeIt);
    }

    if (pendingOutput)
    {
      // Try to apply pending output
      auto const pending = *pendingOutput;
      owner->setOutput(pending.backend, pending.deviceId, pending.profile);

      if (!pendingOutput)
      {
        AUDIO_LOG_INFO("Player: Pending output {}:{} ({}) successfully restored",
                       pending.backend,
                       pending.deviceId,
                       pending.profile);
      }
    }
    if (onDevicesChanged)
    {
      onDevicesChanged(cachedBackends);
    }
  }

  void Player::Impl::handleSystemGraphChanged(Player* owner, flow::Graph const& graph, std::uint64_t generation)
  {
    if (generation != playbackGeneration)
    {
      return;
    }

    cachedSystemGraph = graph;
    updateMergedGraph();
    if (onQualityChanged)
    {
      auto const playerStatus = owner->status();
      onQualityChanged(playerStatus.quality, playerStatus.isReady);
    }
  }

  void Player::Impl::updateMergedGraph()
  {
    auto const& rs = cachedRouteStatus.state;

    mergedGraph = flow::Graph{
      .nodes =
        {
          flow::Node{.id = "ao-decoder",
                     .type = flow::NodeType::Decoder,
                     .name = "Decoder",
                     .optFormat = rs.decoderOutputFormat,
                     .isLossySource = rs.isLossySource},
          flow::Node{
            .id = "ao-engine", .type = flow::NodeType::Engine, .name = "Engine", .optFormat = rs.engineOutputFormat},
        },
      .connections =
        {
          flow::Connection{.sourceId = "ao-decoder", .destId = "ao-engine", .isActive = true},
        },
    };

    auto const optEngineFormat = rs.engineOutputFormat;

    for (auto node : cachedSystemGraph.nodes)
    {
      if (!node.optFormat && optEngineFormat.sampleRate > 0)
      {
        node.optFormat = optEngineFormat;
      }

      mergedGraph.nodes.push_back(node);
    }

    for (auto const& link : cachedSystemGraph.connections)
    {
      mergedGraph.connections.push_back(link);
    }

    auto streamNodeId = std::string{};

    for (auto const& node : cachedSystemGraph.nodes)
    {
      if (node.type == flow::NodeType::Stream)
      {
        streamNodeId = node.id;
        break;
      }
    }

    if (!streamNodeId.empty())
    {
      mergedGraph.connections.push_back({.sourceId = "ao-engine", .destId = streamNodeId, .isActive = true});
    }

    qualityResult = analyzeAudioQuality(mergedGraph);
  }

  Player::Player()
    : _impl{std::make_unique<Impl>()}
  {
    // Start with a NullBackend until a provider provides something real
    _impl->engine = std::make_unique<Engine>(std::make_unique<NullBackend>(),
                                             Device{.id = DeviceId{"null"},
                                                    .displayName = "None",
                                                    .description = "No audio output selected",
                                                    .backendId = kBackendNone,
                                                    .capabilities = {}});

    _impl->engine->setOnTrackEnded(
      [this]()
      {
        if (_impl->onTrackEnded)
        {
          _impl->onTrackEnded();
        }
      });

    _impl->engine->setOnRouteChanged(
      [this](Engine::RouteStatus const& status)
      {
        // Capture generation to prevent stale updates
        auto const generation = _impl->playbackGeneration;

        handleRouteChanged(status, generation);
      });
  }

  void Player::setOnTrackEnded(std::function<void()> callback)
  {
    _impl->onTrackEnded = std::move(callback);
  }

  void Player::setOnDevicesChanged(std::function<void(std::vector<IBackendProvider::Status> const&)> callback)
  {
    _impl->onDevicesChanged = std::move(callback);
  }

  void Player::setOnQualityChanged(std::function<void(ao::audio::Quality quality, bool ready)> callback)
  {
    _impl->onQualityChanged = std::move(callback);
  }

  Player::~Player() = default;

  void Player::addProvider(std::unique_ptr<IBackendProvider> provider)
  {
    if (!provider)
    {
      return;
    }

    auto record = std::make_unique<Impl::ProviderRecord>();
    record->provider = std::move(provider);

    auto* const providerPtr = record->provider.get();
    auto* const recordPtr = record.get();

    _impl->providers.push_back(std::move(record));

    recordPtr->subscription = providerPtr->subscribeDevices(
      [this, providerPtr, recordPtr](std::vector<Device> const& devices)
      {
        recordPtr->devices = devices;
        _impl->handleDevicesChanged(this, providerPtr, devices);
        return true;
      });
  }

  void Player::play(TrackPlaybackDescriptor const& descriptor)
  {
    if (!isReady())
    {
      AUDIO_LOG_WARN("Player: Playback ignored because audio backend is not ready (pending discovery)");
      return;
    }

    _impl->playbackGeneration++;
    _impl->cachedRouteStatus = {};
    _impl->cachedSystemGraph = {};
    _impl->mergedGraph = {};
    _impl->qualityResult = {};
    _impl->graphSubscription.reset();
    _impl->currentTrack = descriptor;
    _impl->engine->play(descriptor);
  }

  void Player::setOutput(BackendId const& backend, DeviceId const& deviceId, ProfileId const& profile)
  {
    auto const currentSnap = _impl->engine->status();

    // 1. Check if we already have this output active
    if (backend == currentSnap.backendId && profile == currentSnap.profileId && deviceId == currentSnap.currentDeviceId)
    {
      _impl->pendingOutput.reset();
      return;
    }

    // 2. Find the Device matching the kind and id from our cache
    auto allDevicesCopy = std::vector<Device>{};
    {
      std::lock_guard<std::mutex> lock(_impl->backendsMutex);
      allDevicesCopy = _impl->allDevices;
    }
    auto const it = std::ranges::find_if(
      allDevicesCopy, [&](Device const& dev) { return dev.backendId == backend && dev.id == deviceId; });

    if (it == allDevicesCopy.end())
    {
      // If we don't have it yet, store it as pending.
      _impl->pendingOutput = Impl::PendingOutput{.backend = backend, .deviceId = deviceId, .profile = profile};
      AUDIO_LOG_DEBUG("Player: Requested output {}:{} not yet available, pending discovery", backend, deviceId);
      return;
    }

    // Found it! Clear any pending output.
    _impl->pendingOutput.reset();

    // 3. Find the provider object that can handle this BackendId
    auto const recordIt = std::ranges::find_if(
      _impl->providers, [&](auto const& record) { return record->provider->status().metadata.id == backend; });

    if (recordIt == _impl->providers.end())
    {
      AUDIO_LOG_ERROR("Player: No provider found for backend {}", backend);
      return;
    }

    // 4. Create the backend and swap it in the engine
    auto const& device = *it;
    auto newBackend = (*recordIt)->provider->createBackend(device, profile);
    _impl->activeManager = (*recordIt)->provider.get();
    _impl->playbackGeneration++;
    _impl->engine->setBackend(std::move(newBackend), device);
  }

  void Player::pause()
  {
    _impl->engine->pause();
  }

  void Player::resume()
  {
    _impl->engine->resume();
  }

  void Player::stop()
  {
    _impl->playbackGeneration++;
    _impl->cachedRouteStatus = {};
    _impl->cachedSystemGraph = {};
    _impl->mergedGraph = {};
    _impl->qualityResult = {};
    _impl->graphSubscription.reset();
    _impl->currentTrack.reset();
    _impl->engine->stop();
  }

  void Player::seek(std::uint32_t positionMs)
  {
    _impl->engine->seek(positionMs);
  }

  void Player::setVolume(float vol)
  {
    _impl->engine->setVolume(vol);
  }

  void Player::setMuted(bool muted)
  {
    _impl->engine->setMuted(muted);
  }

  void Player::toggleMute()
  {
    auto const engineStatus = _impl->engine->status();
    setMuted(!engineStatus.muted);
  }

  Player::Status Player::status() const
  {
    auto status = Player::Status{};
    status.engine = _impl->engine->status();

    if (_impl->currentTrack)
    {
      status.trackTitle = _impl->currentTrack->title;
      status.trackArtist = _impl->currentTrack->artist;
    }

    {
      std::lock_guard<std::mutex> lock(_impl->backendsMutex);
      status.availableBackends = _impl->cachedBackends;
    }
    status.flow = _impl->mergedGraph;
    status.isReady = isReady();

    status.volume = status.engine.volume;
    status.muted = status.engine.muted;
    status.volumeAvailable = status.engine.volumeAvailable;

    status.quality = _impl->qualityResult.quality;
    status.qualityTooltip = _impl->qualityResult.tooltip;
    return status;
  }

  Transport Player::transport() const
  {
    return _impl->engine->transport();
  }

  bool Player::isReady() const
  {
    return _impl->engine->backendId() != kBackendNone && !_impl->pendingOutput.has_value();
  }

  void Player::handleRouteChanged(Engine::RouteStatus const& status, std::uint64_t generation)
  {
    if (generation != _impl->playbackGeneration)
    {
      return;
    }

    _impl->cachedRouteStatus = status;

    // Check if we have a valid anchor and manager to subscribe to the system graph
    if (_impl->cachedRouteStatus.optAnchor && _impl->activeManager != nullptr)
    {
      if (!_impl->graphSubscription)
      {
        _impl->graphSubscription = _impl->activeManager->subscribeGraph(
          _impl->cachedRouteStatus.optAnchor->id,
          [this, generation](flow::Graph const& graph) { _impl->handleSystemGraphChanged(this, graph, generation); });
      }
    }
    else
    {
      _impl->graphSubscription.reset();
      _impl->cachedSystemGraph = {};
    }

    _impl->updateMergedGraph();
    if (_impl->onQualityChanged)
    {
      auto const playerStatus = this->status();
      _impl->onQualityChanged(playerStatus.quality, playerStatus.isReady);
    }
  }

  std::uint64_t Player::playbackGeneration() const noexcept
  {
    return _impl->playbackGeneration;
  }
} // namespace ao::audio
