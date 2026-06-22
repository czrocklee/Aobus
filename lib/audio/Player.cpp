// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/AudioCodec.h>
#include <ao/async/Executor.h>
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
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace ao::audio
{
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

    // Teardown gate shared by executor-marshalled callbacks. Foreign threads
    // may enqueue work after teardown begins, but queued work checks this gate
    // before touching Impl and becomes a no-op once shutdown() has run.
    struct CallbackGate final
    {
      std::atomic<bool> shuttingDown{false};

      bool acceptsCallbacks() const noexcept { return !shuttingDown.load(std::memory_order_acquire); }
      void shutdown() noexcept { shuttingDown.store(true, std::memory_order_release); }
    };

    explicit Impl(async::IExecutor& exec)
      : executor{exec}
    {
    }

    Impl(Impl const&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl const&) = delete;
    Impl& operator=(Impl&&) = delete;

    ~Impl()
    {
      gatePtr->shutdown();

      // Teardown order:
      //   1. Unsubscribe graph and device callbacks so no new callbacks fire.
      //   2. Shut down providers' async activity (monitor threads, PW event loops) so no
      //      in-flight callbacks can race with engine/backend destruction.
      //   3. Destroy the engine (which closes/destroys the active backend). At this point
      //      the backend may still touch provider-owned state (e.g. AlsaGraphRegistry),
      //      which is why providers are still alive.
      //   4. Destroy providers last.
      graphSubscription.reset();

      for (auto& record : providers)
      {
        record->subscription.reset();
      }

      for (auto& record : providers)
      {
        record->providerPtr->shutdown();
      }

      // Defensive: prevent dangling use if Engine teardown somehow triggers handleRouteChanged.
      activeManager = nullptr;
      enginePtr.reset();
      providers.clear();
    }

    async::IExecutor& executor;
    std::atomic<std::uint64_t> playbackGeneration{1};
    std::vector<std::unique_ptr<ProviderRecord>> providers;
    std::optional<PendingOutput> optPendingOutput;
    IBackendProvider* activeManager = nullptr;
    Subscription graphSubscription;
    std::unique_ptr<Engine> enginePtr;
    std::shared_ptr<CallbackGate> gatePtr = std::make_shared<CallbackGate>();

    mutable std::mutex backendsMutex;
    mutable std::vector<IBackendProvider::Status> cachedBackends;
    mutable std::vector<Device> allDevices;

    mutable std::mutex graphMutex;
    Engine::RouteStatus cachedRouteStatus;
    flow::Graph cachedSystemGraph;
    flow::Graph mergedGraph;
    QualityResult qualityResult;
    std::function<void()> onTrackEnded;
    std::function<void()> onStateChanged;
    std::function<void(std::vector<IBackendProvider::Status> const&)> onDevicesChanged;
    std::function<void(Quality, bool)> onQualityChanged;

    void handleDevicesChanged(Player* owner, IBackendProvider* provider, std::vector<Device> const& devices);
    void handleSystemGraphChanged(Player* owner, flow::Graph const& graph, std::uint64_t generation);
    void updateMergedGraph();

    template<typename Task>
    static void dispatchInternal(async::IExecutor& executor, std::shared_ptr<CallbackGate> gatePtr, Task task)
    {
      executor.dispatch(
        [gatePtr = std::move(gatePtr), task = std::move(task)] mutable
        {
          if (!gatePtr->acceptsCallbacks())
          {
            return;
          }

          task();
        });
    }

    // Marshal one of the outward on* callback slots onto the executor thread.
    // The task copies the user callback while the gate is open, then invokes the
    // copy without holding any teardown wait state. This keeps queued tasks safe
    // after teardown and avoids self-deadlock if a user callback destroys Player.
    template<typename Slot, typename... Args>
    void dispatchOutward(Slot Impl::* slot, Args... args)
    {
      executor.dispatch(
        [this, slot, gatePtr = gatePtr, args = std::make_tuple(std::move(args)...)] mutable
        {
          if (!gatePtr->acceptsCallbacks())
          {
            return;
          }

          auto callback = std::decay_t<decltype(this->*slot)>{};
          callback = this->*slot;

          if (callback)
          {
            std::apply(callback, std::move(args));
          }
        });
    }
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

    auto snapshot = std::vector<IBackendProvider::Status>{};
    {
      auto const lock = std::scoped_lock{backendsMutex};
      snapshot = cachedBackends;
    }

    dispatchOutward(&Impl::onDevicesChanged, std::move(snapshot));
  }

  void Player::Impl::handleSystemGraphChanged(Player* owner, flow::Graph const& graph, std::uint64_t generation)
  {
    if (generation != playbackGeneration.load(std::memory_order_acquire))
    {
      return;
    }

    {
      auto const lock = std::scoped_lock{graphMutex};
      cachedSystemGraph = graph;
      updateMergedGraph();
    }

    auto const playerStatus = owner->status();
    dispatchOutward(&Impl::onQualityChanged, playerStatus.quality, playerStatus.isReady);
  }

  void Player::Impl::updateMergedGraph()
  {
    auto const& rs = cachedRouteStatus.state;

    // Label the source node with the detected codec (e.g. "FLAC"); fall back to a
    // generic name before a track is decoded or when the codec is unknown.
    auto const codecName = audioCodecName(rs.codec);
    auto sourceName = codecName.empty() ? std::string{"Source"} : std::string{codecName};

    mergedGraph = flow::Graph{
      .nodes =
        {
          // The source node carries the track's native format and lossy-ness; the
          // decoder node carries the PCM it emits. Keeping them separate makes any
          // container padding (source -> decoder bit-depth change) a first-class
          // transition rather than a hidden property of a single conflated node.
          flow::Node{.id = "ao-source",
                     .type = flow::NodeType::Source,
                     .name = std::move(sourceName),
                     .optFormat = rs.sourceFormat,
                     .isLossySource = rs.isLossySource},
          flow::Node{.id = "ao-decoder",
                     .type = flow::NodeType::Decoder,
                     .name = "Decoded PCM",
                     .optFormat = rs.decoderOutputFormat},
          flow::Node{.id = "ao-engine",
                     .type = flow::NodeType::Engine,
                     .name = "Aobus Engine",
                     .optFormat = rs.engineOutputFormat},
        },
      .connections =
        {
          flow::Connection{.sourceId = "ao-source", .destId = "ao-decoder", .isActive = true},
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

  Player::Player(async::IExecutor& executor)
    : _implPtr{std::make_unique<Impl>(executor)}
  {
    // Start with a NullBackend until a provider provides something real
    _implPtr->enginePtr = std::make_unique<Engine>(std::make_unique<NullBackend>(),
                                                   Device{.id = DeviceId{"null"},
                                                          .displayName = "None",
                                                          .description = "No audio output selected",
                                                          .backendId = kBackendNone,
                                                          .capabilities = {}});

    _implPtr->enginePtr->setOnTrackEnded(
      [this, gatePtr = _implPtr->gatePtr, &executor = _implPtr->executor]
      { Impl::dispatchInternal(executor, gatePtr, [this] { _implPtr->dispatchOutward(&Impl::onTrackEnded); }); });

    _implPtr->enginePtr->setOnStateChanged([this, gatePtr = _implPtr->gatePtr, &executor = _implPtr->executor]
                                           { _implPtr->dispatchOutward(&Impl::onStateChanged); });

    auto* const gen = &_implPtr->playbackGeneration;
    _implPtr->enginePtr->setOnRouteChanged(
      [this, gatePtr = _implPtr->gatePtr, &executor = _implPtr->executor, gen](Engine::RouteStatus const& status)
      {
        // Capture generation at the Engine event boundary so a route event
        // queued before a later stop/play cannot be applied to the new session.
        auto const generation = gen->load(std::memory_order_acquire);
        Impl::dispatchInternal(
          executor, gatePtr, [this, status, generation] { handleRouteChanged(status, generation); });
      });
  }

  void Player::setOnTrackEnded(std::function<void()> callback)
  {
    _implPtr->onTrackEnded = std::move(callback);
  }

  void Player::setOnStateChanged(std::function<void()> callback)
  {
    _implPtr->onStateChanged = std::move(callback);
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

    auto* const provider = recordPtr->providerPtr.get();
    auto* const record = recordPtr.get();
    _implPtr->providers.push_back(std::move(recordPtr));

    record->subscription = provider->subscribeDevices(
      [this, provider, gatePtr = _implPtr->gatePtr, &executor = _implPtr->executor](std::vector<Device> const& devices)
      {
        Impl::dispatchInternal(
          executor, gatePtr, [this, provider, devices] { _implPtr->handleDevicesChanged(this, provider, devices); });
        return true;
      });
  }

  void Player::play(PlaybackInput const& input)
  {
    if (!isReady())
    {
      AUDIO_LOG_WARN("Player: Playback ignored because audio backend is not ready (pending discovery)");
      return;
    }

    _implPtr->playbackGeneration.fetch_add(1, std::memory_order_acq_rel);
    {
      auto const lock = std::scoped_lock{_implPtr->graphMutex};
      _implPtr->cachedRouteStatus = {};
      _implPtr->cachedSystemGraph = {};
      _implPtr->mergedGraph = {};
      _implPtr->qualityResult = {};
    }
    _implPtr->graphSubscription.reset();
    _implPtr->enginePtr->play(input);
  }

  void Player::setOutput(BackendId const& backend, DeviceId const& deviceId, ProfileId const& profile)
  {
    if (auto const currentSnap = _implPtr->enginePtr->status();
        backend == currentSnap.backendId && profile == currentSnap.profileId && deviceId == currentSnap.currentDeviceId)
    {
      _implPtr->optPendingOutput.reset();
      _implPtr->dispatchOutward(&Impl::onStateChanged);
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
      _implPtr->dispatchOutward(&Impl::onStateChanged);
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
      _implPtr->dispatchOutward(&Impl::onStateChanged);
      return;
    }

    // 4. Create the backend and swap it in the engine
    auto const& device = *it;
    auto newBackendPtr = (*recordIt)->providerPtr->createBackend(device, profile);
    _implPtr->activeManager = (*recordIt)->providerPtr.get();
    _implPtr->playbackGeneration.fetch_add(1, std::memory_order_acq_rel);
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
    _implPtr->playbackGeneration.fetch_add(1, std::memory_order_acq_rel);
    {
      auto const lock = std::scoped_lock{_implPtr->graphMutex};
      _implPtr->cachedRouteStatus = {};
      _implPtr->cachedSystemGraph = {};
      _implPtr->mergedGraph = {};
      _implPtr->qualityResult = {};
    }
    _implPtr->graphSubscription.reset();
    _implPtr->enginePtr->stop();
  }

  void Player::seek(std::chrono::milliseconds offset)
  {
    _implPtr->enginePtr->seek(offset);
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

    {
      auto const lock = std::scoped_lock{_implPtr->backendsMutex};
      status.availableBackends = _implPtr->cachedBackends;
    }

    {
      auto const lock = std::scoped_lock{_implPtr->graphMutex};
      status.flow = _implPtr->mergedGraph;
      status.quality = _implPtr->qualityResult.quality;
      status.qualityAssessments = _implPtr->qualityResult.assessments;
    }
    status.isReady = isReady();

    status.volume = status.engine.volume;
    status.muted = status.engine.muted;
    status.volumeAvailable = status.engine.volumeAvailable;
    status.volumeIsHardwareAssisted = status.engine.volumeIsHardwareAssisted;
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
    if (generation != _implPtr->playbackGeneration.load(std::memory_order_acquire))
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
        _implPtr->graphSubscription = _implPtr->activeManager->subscribeGraph(
          status.optAnchor->id,
          [this, generation, gatePtr = _implPtr->gatePtr, &executor = _implPtr->executor](flow::Graph const& graph)
          {
            Impl::dispatchInternal(executor,
                                   gatePtr,
                                   [this, graph, generation]
                                   { _implPtr->handleSystemGraphChanged(this, graph, generation); });
          });
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

    auto const playerStatus = this->status();
    _implPtr->dispatchOutward(&Impl::onQualityChanged, playerStatus.quality, playerStatus.isReady);
  }

  std::uint64_t Player::playbackGeneration() const noexcept
  {
    return _implPtr->playbackGeneration.load(std::memory_order_acquire);
  }
} // namespace ao::audio
