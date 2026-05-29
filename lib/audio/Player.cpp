// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/audio/Backend.h>
#include <ao/audio/Engine.h>
#include <ao/audio/IBackendProvider.h>
#include <ao/audio/NullBackend.h>
#include <ao/audio/Player.h>
#include <ao/audio/QualityAnalyzer.h>
#include <ao/audio/Subscription.h>
#include <ao/audio/Types.h>
#include <ao/audio/flow/Graph.h>
#include <ao/utility/Log.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace ao::audio
{
  namespace
  {
  }

  struct Player::Impl final
  {
    struct ProviderRecord
    {
      std::unique_ptr<IBackendProvider> providerPtr;
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
    Impl& operator=(Impl const&) = delete;
    Impl& operator=(Impl&&) = delete;

    ~Impl()
    {
      // Order matters: stop PipeWire threads before their callback targets are destroyed.
      graphSubscription.reset(); // 1. remove subscription from PipeWireMonitor (still alive)
      enginePtr.reset();         // 2. stop PipeWire playback thread
      providers.clear();         // 3. stop PipeWire monitor thread
    }

    std::uint64_t playbackGeneration = 1;
    std::vector<std::unique_ptr<ProviderRecord>> providers;
    std::optional<PendingOutput> optPendingOutput;
    IBackendProvider* activeManager = nullptr;
    Subscription graphSubscription;
    std::unique_ptr<Engine> enginePtr;

    mutable std::mutex backendsMutex;
    mutable std::vector<IBackendProvider::Status> cachedBackends;
    mutable std::vector<Device> allDevices;

    mutable std::mutex graphMutex;
    Engine::RouteStatus cachedRouteStatus;
    flow::Graph cachedSystemGraph;
    flow::Graph mergedGraph;
    QualityResult qualityResult;
    std::optional<TrackPlaybackDescriptor> optCurrentTrack;

    std::function<void()> onTrackEnded;
    std::function<void(std::vector<IBackendProvider::Status> const&)> onDevicesChanged;
    std::function<void(Quality, bool)> onQualityChanged;

    void handleDevicesChanged(Player* owner, IBackendProvider* provider, std::vector<Device> const& devices);
    void handleSystemGraphChanged(Player* owner, flow::Graph const& graph, std::uint64_t generation);
    void updateMergedGraph();
  };

  void Player::Impl::handleDevicesChanged(Player* owner, IBackendProvider* provider, std::vector<Device> const& devices)
  {
    // Update individual provider cache
    auto const it =
      std::ranges::find_if(providers, [&](auto const& record) { return record->providerPtr.get() == provider; });

    if (it != providers.end())
    {
      (*it)->devices = devices;
    }

    // Rebuild global cache from all providers
    auto allDevicesList = std::vector<Device>{};
    auto snapshots = std::vector<IBackendProvider::Status>{};

    for (auto const& record : providers)
    {
      auto status = record->providerPtr->status();
      // The provider status might have its own internal device list, but we use the one from the subscription for
      // consistency
      status.devices = record->devices;
      snapshots.push_back(std::move(status));
      allDevicesList.insert(allDevicesList.end(), record->devices.begin(), record->devices.end());
    }

    {
      auto const lock = std::scoped_lock{backendsMutex};
      cachedBackends = std::move(snapshots);
      allDevices = std::move(allDevicesList);
    }

    // Keep the engine's current device capabilities up-to-date
    auto const currentSnap = enginePtr->status();
    auto allDevicesCopy = std::vector<Device>{};
    {
      auto const lock = std::scoped_lock{backendsMutex};
      allDevicesCopy = allDevices;
    }

    auto const activeIt =
      std::ranges::find_if(allDevicesCopy,
                           [&](Device const& dev)
                           { return dev.backendId == currentSnap.backendId && dev.id == currentSnap.currentDeviceId; });

    if (activeIt != allDevicesCopy.end())
    {
      enginePtr->updateDevice(*activeIt);
    }

    if (optPendingOutput)
    {
      // Try to apply pending output
      auto const pending = PendingOutput{*optPendingOutput};
      owner->setOutput(pending.backend, pending.deviceId, pending.profile);

      if (!optPendingOutput)
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

    {
      auto const lock = std::scoped_lock{graphMutex};
      cachedSystemGraph = graph;
      updateMergedGraph();
    }

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

    auto const optEngineFormat = Format{rs.engineOutputFormat};

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
    : _implPtr{std::make_unique<Impl>()}
  {
    // Start with a NullBackend until a provider provides something real
    _implPtr->enginePtr = std::make_unique<Engine>(std::make_unique<NullBackend>(),
                                                   Device{.id = DeviceId{"null"},
                                                          .displayName = "None",
                                                          .description = "No audio output selected",
                                                          .backendId = kBackendNone,
                                                          .capabilities = {}});

    _implPtr->enginePtr->setOnTrackEnded(
      [this]
      {
        if (_implPtr->onTrackEnded)
        {
          _implPtr->onTrackEnded();
        }
      });

    _implPtr->enginePtr->setOnRouteChanged(
      [this](Engine::RouteStatus const& status)
      {
        // Capture generation to prevent stale updates
        auto const generation = _implPtr->playbackGeneration;

        handleRouteChanged(status, generation);
      });
  }

  void Player::setOnTrackEnded(std::function<void()> callback)
  {
    _implPtr->onTrackEnded = std::move(callback);
  }

  void Player::setOnDevicesChanged(std::function<void(std::vector<IBackendProvider::Status> const&)> callback)
  {
    _implPtr->onDevicesChanged = std::move(callback);
  }

  void Player::setOnQualityChanged(std::function<void(Quality quality, bool ready)> callback)
  {
    _implPtr->onQualityChanged = std::move(callback);
  }

  Player::~Player() = default;

  void Player::addProvider(std::unique_ptr<IBackendProvider> providerPtr)
  {
    if (!providerPtr)
    {
      return;
    }

    auto recordPtr = std::make_unique<Impl::ProviderRecord>();
    recordPtr->providerPtr = std::move(providerPtr);

    auto* const rawProviderPtr = recordPtr->providerPtr.get();
    auto* const rawRecordPtr = recordPtr.get();

    _implPtr->providers.push_back(std::move(recordPtr));

    rawRecordPtr->subscription = rawProviderPtr->subscribeDevices(
      [this, rawProviderPtr, rawRecordPtr](std::vector<Device> const& devices)
      {
        rawRecordPtr->devices = devices;
        _implPtr->handleDevicesChanged(this, rawProviderPtr, devices);
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

    _implPtr->playbackGeneration++;
    {
      auto const lock = std::scoped_lock{_implPtr->graphMutex};
      _implPtr->cachedRouteStatus = {};
      _implPtr->cachedSystemGraph = {};
      _implPtr->mergedGraph = {};
      _implPtr->qualityResult = {};
    }
    _implPtr->graphSubscription.reset();
    _implPtr->optCurrentTrack = descriptor;
    _implPtr->enginePtr->play(descriptor);
  }

  void Player::setOutput(BackendId const& backend, DeviceId const& deviceId, ProfileId const& profile)
  {
    if (auto const currentSnap = _implPtr->enginePtr->status();
        backend == currentSnap.backendId && profile == currentSnap.profileId && deviceId == currentSnap.currentDeviceId)
    {
      _implPtr->optPendingOutput.reset();
      return;
    }

    // 2. Find the Device matching the kind and id from our cache
    auto allDevicesCopy = std::vector<Device>{};
    {
      auto const lock = std::scoped_lock{_implPtr->backendsMutex};
      allDevicesCopy = _implPtr->allDevices;
    }

    auto const it = std::ranges::find_if(
      allDevicesCopy, [&](Device const& dev) { return dev.backendId == backend && dev.id == deviceId; });

    if (it == allDevicesCopy.end())
    {
      // If we don't have it yet, store it as pending.
      _implPtr->optPendingOutput = Impl::PendingOutput{.backend = backend, .deviceId = deviceId, .profile = profile};
      AUDIO_LOG_DEBUG("Player: Requested output {}:{} not yet available, pending discovery", backend, deviceId);
      return;
    }

    // Found it! Clear any pending output.
    _implPtr->optPendingOutput.reset();

    // 3. Find the provider object that can handle this BackendId
    auto const recordIt = std::ranges::find_if(
      _implPtr->providers, [&](auto const& record) { return record->providerPtr->status().metadata.id == backend; });

    if (recordIt == _implPtr->providers.end())
    {
      AUDIO_LOG_ERROR("Player: No provider found for backend {}", backend);
      return;
    }

    // 4. Create the backend and swap it in the engine
    auto const& device = *it;
    auto newBackendPtr = (*recordIt)->providerPtr->createBackend(device, profile);
    _implPtr->activeManager = (*recordIt)->providerPtr.get();
    _implPtr->playbackGeneration++;
    _implPtr->enginePtr->setBackend(std::move(newBackendPtr), device);
  }

  void Player::pause()
  {
    _implPtr->enginePtr->pause();
  }

  void Player::resume()
  {
    _implPtr->enginePtr->resume();
  }

  void Player::stop()
  {
    _implPtr->playbackGeneration++;
    {
      auto const lock = std::scoped_lock{_implPtr->graphMutex};
      _implPtr->cachedRouteStatus = {};
      _implPtr->cachedSystemGraph = {};
      _implPtr->mergedGraph = {};
      _implPtr->qualityResult = {};
    }
    _implPtr->graphSubscription.reset();
    _implPtr->optCurrentTrack.reset();
    _implPtr->enginePtr->stop();
  }

  void Player::seek(std::uint32_t positionMs)
  {
    _implPtr->enginePtr->seek(positionMs);
  }

  void Player::setVolume(float vol)
  {
    _implPtr->enginePtr->setVolume(vol);
  }

  void Player::setMuted(bool muted)
  {
    _implPtr->enginePtr->setMuted(muted);
  }

  void Player::toggleMute()
  {
    auto const engineStatus = _implPtr->enginePtr->status();
    setMuted(!engineStatus.muted);
  }

  Player::Status Player::status() const
  {
    auto status = Player::Status{};
    status.engine = _implPtr->enginePtr->status();

    if (_implPtr->optCurrentTrack)
    {
      status.trackTitle = _implPtr->optCurrentTrack->title;
      status.trackArtist = _implPtr->optCurrentTrack->artist;
    }

    {
      auto const lock = std::scoped_lock{_implPtr->backendsMutex};
      status.availableBackends = _implPtr->cachedBackends;
    }

    {
      auto const lock = std::scoped_lock{_implPtr->graphMutex};
      status.flow = _implPtr->mergedGraph;
      status.quality = _implPtr->qualityResult.quality;
      status.qualityTooltip = _implPtr->qualityResult.tooltip;
    }
    status.isReady = isReady();

    status.volume = status.engine.volume;
    status.muted = status.engine.muted;
    status.volumeAvailable = status.engine.volumeAvailable;
    return status;
  }

  Transport Player::transport() const
  {
    return _implPtr->enginePtr->transport();
  }

  bool Player::isReady() const
  {
    return _implPtr->enginePtr->backendId() != kBackendNone && !_implPtr->optPendingOutput;
  }

  void Player::handleRouteChanged(Engine::RouteStatus const& status, std::uint64_t generation)
  {
    if (generation != _implPtr->playbackGeneration)
    {
      return;
    }

    auto lock = std::unique_lock{_implPtr->graphMutex};
    _implPtr->cachedRouteStatus = status;
    lock.unlock();

    // Check if we have a valid anchor and manager to subscribe to the system graph
    auto const hasRoute = status.optAnchor && _implPtr->activeManager != nullptr;

    if (hasRoute)
    {
      if (!_implPtr->graphSubscription)
      {
        _implPtr->graphSubscription =
          _implPtr->activeManager->subscribeGraph(status.optAnchor->id,
                                                  [this, generation](flow::Graph const& graph)
                                                  { _implPtr->handleSystemGraphChanged(this, graph, generation); });
      }
    }
    else
    {
      _implPtr->graphSubscription.reset();
    }

    lock.lock();

    if (!hasRoute)
    {
      _implPtr->cachedSystemGraph = {};
    }

    _implPtr->updateMergedGraph();
    lock.unlock();

    if (_implPtr->onQualityChanged)
    {
      auto const playerStatus = this->status();
      _implPtr->onQualityChanged(playerStatus.quality, playerStatus.isReady);
    }
  }

  std::uint64_t Player::playbackGeneration() const noexcept
  {
    return _implPtr->playbackGeneration;
  }
} // namespace ao::audio
