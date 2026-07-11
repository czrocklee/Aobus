// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/AudioCodec.h>
#include <ao/Error.h>
#include <ao/async/Executor.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/BackendProvider.h>
#include <ao/audio/Device.h>
#include <ao/audio/Engine.h>
#include <ao/audio/NullBackend.h>
#include <ao/audio/Player.h>
#include <ao/audio/QualityAnalyzer.h>
#include <ao/audio/Subscription.h>
#include <ao/audio/Transport.h>
#include <ao/audio/flow/Graph.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <expected>
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
  namespace
  {
    QualityResult qualityResultFromStatus(Player::Status const& status)
    {
      return QualityResult{.sourceQuality = status.sourceQuality,
                           .pipelineQuality = status.pipelineQuality,
                           .overall = status.quality,
                           .fullyVerified = status.qualityFullyVerified,
                           .assessments = status.qualityAssessments};
    }
  } // namespace

  struct Player::Impl final : std::enable_shared_from_this<Player::Impl>
  {
    struct ProviderRecord
    {
      std::unique_ptr<BackendProvider> providerPtr;
      Subscription subscription;
      std::vector<Device> devices;
    };

    struct PendingOutputDeviceSelection
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

      bool canAcceptCallbacks() const noexcept { return !shuttingDown.load(std::memory_order_acquire); }
      bool shutdown() noexcept { return !shuttingDown.exchange(true, std::memory_order_acq_rel); }
    };

    explicit Impl(async::Executor& exec)
      : executor{exec}
    {
    }

    Impl(Impl const&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl const&) = delete;
    Impl& operator=(Impl&&) = delete;

    ~Impl() { shutdown(); }

    void shutdown() noexcept
    {
      if (!gatePtr->shutdown())
      {
        return;
      }

      // Teardown order:
      //   1. Unsubscribe graph and device callbacks so no new callbacks fire.
      //   2. Shut down providers' async activity (monitor threads, PW event loops) so no
      //      in-flight callbacks can race with engine/backend destruction.
      //   3. Stop the engine, which closes/destroys the active backend while
      //      provider-owned state (e.g. AlsaGraphRegistry) is still alive.
      //   4. Retain the stopped Engine wrapper and providers until Impl itself
      //      is reclaimed, so a reentrant callback can finish unwinding safely.
      graphSubscription.reset();

      for (auto& recordPtr : providers)
      {
        recordPtr->subscription.reset();
      }

      for (auto& recordPtr : providers)
      {
        recordPtr->providerPtr->shutdown();
      }

      // Defensive: prevent dangling use if Engine teardown somehow triggers handleRouteChanged.
      activeBackendProvider = nullptr;

      if (enginePtr)
      {
        enginePtr->shutdown();
      }

      // Retain the stopped Engine wrapper until Impl is reclaimed. A provider
      // callback that passed the atomic gate immediately before shutdown may
      // still be unwinding through deferInternal(); Engine::defer safely drops
      // work after its event queue has stopped.
    }

    async::Executor& executor;
    std::atomic<std::uint64_t> playbackGeneration{1};
    std::atomic<std::uint64_t> audioCallbackGenerationFloor{1};
    std::vector<std::unique_ptr<ProviderRecord>> providers;
    std::optional<PendingOutputDeviceSelection> optPendingOutputDeviceSelection;
    BackendProvider* activeBackendProvider = nullptr;
    Subscription graphSubscription;
    std::unique_ptr<Engine> enginePtr;
    std::shared_ptr<CallbackGate> gatePtr = std::make_shared<CallbackGate>();

    mutable std::mutex backendsMutex;
    mutable std::vector<BackendProvider::Status> cachedBackends;
    mutable std::vector<Device> allDevices;

    mutable std::mutex graphMutex;
    Engine::RouteStatus cachedRouteStatus;
    flow::Graph cachedSystemGraph;
    flow::Graph mergedGraph;
    QualityResult qualityResult;
    std::function<void(Engine::TrackEnded const&)> onTrackEnded;
    std::function<void(Engine::TrackAdvanced const&)> onTrackAdvanced;
    std::function<void(Engine::PlaybackFailure const&)> onPlaybackFailure;
    std::function<void()> onStateChanged;
    std::function<void(std::vector<BackendProvider::Status> const&)> onOutputDevicesChanged;
    std::function<void(QualityResult const&, bool)> onQualityChanged;

    void connectEngineCallbacks();
    void connectTrackEndedCallback();
    void connectTrackAdvancedCallback();
    void connectPlaybackFailureCallback();
    void connectStateChangedCallback();
    void connectRouteChangedCallback();
    void handleOutputDevicesChanged(BackendProvider* provider, std::vector<Device> const& devices);
    void handleSystemGraphChanged(flow::Graph const& graph, std::uint64_t generation);
    void handleRouteChanged(Engine::RouteStatus const& status, std::uint64_t generation);
    Result<> setOutputDevice(BackendId const& backend, DeviceId const& deviceId, ProfileId const& profile);
    Player::Status snapshot() const;
    bool isReady() const;
    void updateMergedGraph();

    bool acceptsAudioCallback(std::uint64_t generation) const noexcept
    {
      return generation >= audioCallbackGenerationFloor.load(std::memory_order_acquire);
    }

    void acceptAudioBarrier(Engine::PreparedCancellationBarrier barrier) noexcept
    {
      auto floor = audioCallbackGenerationFloor.load(std::memory_order_acquire);

      while (floor < barrier.generation &&
             !audioCallbackGenerationFloor.compare_exchange_weak(
               floor, barrier.generation, std::memory_order_acq_rel, std::memory_order_acquire))
      {
      }
    }

    std::uint64_t resetPlaybackGraph()
    {
      auto const generation = playbackGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
      graphSubscription.reset();

      auto const lock = std::scoped_lock{graphMutex};
      cachedRouteStatus = {};
      cachedSystemGraph = {};
      mergedGraph = {};
      qualityResult = {};
      return generation;
    }

    template<typename Task>
    void dispatchInternal(Task task)
    {
      auto weakSelfPtr = weak_from_this();
      executor.dispatch(
        [weakSelfPtr = std::move(weakSelfPtr), task = std::move(task)] mutable
        {
          auto selfPtr = weakSelfPtr.lock();

          if (!selfPtr || !selfPtr->gatePtr->canAcceptCallbacks())
          {
            return;
          }

          task(*selfPtr);
        });
    }

    // Provider callbacks can be synchronous backend-control callbacks. Hand
    // graph work through Engine's event queue first so even an inline executor
    // cannot run Player/user callbacks while Engine holds its control lock.
    template<typename Task>
    void deferInternal(Task task)
    {
      auto weakSelfPtr = weak_from_this();
      enginePtr->defer(
        [weakSelfPtr = std::move(weakSelfPtr), task = std::move(task)] mutable
        {
          if (auto selfPtr = weakSelfPtr.lock(); selfPtr && selfPtr->gatePtr->canAcceptCallbacks())
          {
            selfPtr->dispatchInternal(std::move(task));
          }
        });
    }

    // Marshal one of the outward on* callback slots onto the executor thread.
    // The task copies the user callback while the gate is open, then invokes the
    // copy without holding any teardown wait state. This keeps queued tasks safe
    // after teardown and avoids self-deadlock if a user callback destroys Player.
    template<typename Slot, typename... Args>
    void dispatchOutward(Slot Impl::* slot, Args... args)
    {
      auto weakSelfPtr = weak_from_this();
      executor.dispatch(
        [weakSelfPtr = std::move(weakSelfPtr), slot, args = std::make_tuple(std::move(args)...)] mutable
        {
          auto selfPtr = weakSelfPtr.lock();

          if (!selfPtr || !selfPtr->gatePtr->canAcceptCallbacks())
          {
            return;
          }

          auto callback = std::decay_t<decltype(selfPtr.get()->*slot)>{};
          callback = selfPtr.get()->*slot;

          if (callback)
          {
            std::apply(callback, std::move(args));
          }
        });
    }
  };

  void Player::Impl::connectEngineCallbacks()
  {
    connectTrackEndedCallback();
    connectTrackAdvancedCallback();
    connectPlaybackFailureCallback();
    connectStateChangedCallback();
    connectRouteChangedCallback();
  }

  void Player::Impl::connectTrackEndedCallback()
  {
    auto const weakImplPtr = weak_from_this();
    enginePtr->setOnTrackEnded(
      [weakImplPtr](Engine::TrackEnded const& event)
      {
        auto implPtr = weakImplPtr.lock();

        if (!implPtr || !implPtr->gatePtr->canAcceptCallbacks())
        {
          return;
        }

        implPtr->dispatchInternal(
          [event](Impl& self)
          {
            if (!self.acceptsAudioCallback(event.generation))
            {
              return;
            }

            if (auto callback = self.onTrackEnded; callback)
            {
              callback(event);
            }
          });
      });
  }

  void Player::Impl::connectTrackAdvancedCallback()
  {
    auto const weakImplPtr = weak_from_this();
    enginePtr->setOnTrackAdvanced(
      [weakImplPtr](Engine::TrackAdvanced const& event)
      {
        auto implPtr = weakImplPtr.lock();

        if (!implPtr || !implPtr->gatePtr->canAcceptCallbacks())
        {
          return;
        }

        implPtr->dispatchInternal(
          [event = Engine::TrackAdvanced{event}](Impl& self)
          {
            if (!self.acceptsAudioCallback(event.generation))
            {
              return;
            }

            std::ignore = self.resetPlaybackGraph();

            if (auto callback = self.onTrackAdvanced; callback)
            {
              callback(event);
            }
          });
      });
  }

  void Player::Impl::connectPlaybackFailureCallback()
  {
    auto const weakImplPtr = weak_from_this();
    enginePtr->setOnPlaybackFailure(
      [weakImplPtr](Engine::PlaybackFailure const& failure)
      {
        if (auto implPtr = weakImplPtr.lock(); implPtr && implPtr->gatePtr->canAcceptCallbacks())
        {
          implPtr->dispatchInternal(
            [failure = Engine::PlaybackFailure{failure}](Impl& self)
            {
              if (!self.acceptsAudioCallback(failure.generation))
              {
                return;
              }

              if (auto callback = self.onPlaybackFailure; callback)
              {
                callback(failure);
              }
            });
        }
      });
  }

  void Player::Impl::connectStateChangedCallback()
  {
    auto const weakImplPtr = weak_from_this();
    enginePtr->setOnStateChanged(
      [weakImplPtr]
      {
        if (auto implPtr = weakImplPtr.lock(); implPtr && implPtr->gatePtr->canAcceptCallbacks())
        {
          implPtr->dispatchOutward(&Impl::onStateChanged);
        }
      });
  }

  void Player::Impl::connectRouteChangedCallback()
  {
    auto const weakImplPtr = weak_from_this();
    enginePtr->setOnRouteChanged(
      [weakImplPtr](Engine::RouteStatus const& status)
      {
        auto implPtr = weakImplPtr.lock();

        if (!implPtr || !implPtr->gatePtr->canAcceptCallbacks())
        {
          return;
        }

        implPtr->dispatchInternal(
          [status = Engine::RouteStatus{status}](Impl& self)
          {
            if (!self.acceptsAudioCallback(status.generation))
            {
              return;
            }

            auto const graphGeneration = self.playbackGeneration.load(std::memory_order_acquire);
            self.handleRouteChanged(status, graphGeneration);
          });
      });
  }

  void Player::Impl::handleOutputDevicesChanged(BackendProvider* provider, std::vector<Device> const& devices)
  {
    // Update individual provider cache
    auto const it =
      std::ranges::find_if(providers, [&](auto const& recordPtr) { return recordPtr->providerPtr.get() == provider; });

    if (it != providers.end())
    {
      (*it)->devices = devices;
    }

    // Rebuild global cache from all providers
    auto allProviderDevices = std::vector<Device>{};
    auto snapshots = std::vector<BackendProvider::Status>{};

    for (auto const& recordPtr : providers)
    {
      auto status = recordPtr->providerPtr->status();
      // The provider status might have its own internal device list, but we use the one from the subscription for
      // consistency
      status.devices = recordPtr->devices;
      snapshots.push_back(std::move(status));
      allProviderDevices.insert(allProviderDevices.end(), recordPtr->devices.begin(), recordPtr->devices.end());
    }

    {
      auto const lock = std::scoped_lock{backendsMutex};
      cachedBackends = std::move(snapshots);
      allDevices = std::move(allProviderDevices);
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

    if (optPendingOutputDeviceSelection)
    {
      // Try to apply pending output
      auto const pending = PendingOutputDeviceSelection{*optPendingOutputDeviceSelection};
      std::ignore = setOutputDevice(pending.backend, pending.deviceId, pending.profile);
    }

    auto snapshot = std::vector<BackendProvider::Status>{};
    {
      auto const lock = std::scoped_lock{backendsMutex};
      snapshot = cachedBackends;
    }

    dispatchOutward(&Impl::onOutputDevicesChanged, std::move(snapshot));
  }

  void Player::Impl::handleSystemGraphChanged(flow::Graph const& graph, std::uint64_t generation)
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

    auto const playerStatus = snapshot();
    dispatchOutward(&Impl::onQualityChanged, qualityResultFromStatus(playerStatus), playerStatus.isReady);
  }

  Result<> Player::Impl::setOutputDevice(BackendId const& backend, DeviceId const& deviceId, ProfileId const& profile)
  {
    if (auto const currentSnap = enginePtr->status();
        backend == currentSnap.backendId && profile == currentSnap.profileId && deviceId == currentSnap.currentDeviceId)
    {
      optPendingOutputDeviceSelection.reset();
      dispatchOutward(&Impl::onStateChanged);
      return {};
    }

    auto const recordIt = std::ranges::find_if(
      providers, [&](auto const& recordPtr) { return recordPtr->providerPtr->status().descriptor.id == backend; });

    if (recordIt == providers.end())
    {
      return makeError(Error::Code::NotFound, "No provider registered for backend " + backend.raw());
    }

    auto allDevicesCopy = std::vector<Device>{};
    {
      auto const lock = std::scoped_lock{backendsMutex};
      allDevicesCopy = allDevices;
    }

    auto const it = std::ranges::find_if(
      allDevicesCopy, [&](Device const& dev) { return dev.backendId == backend && dev.id == deviceId; });

    if (it == allDevicesCopy.end())
    {
      optPendingOutputDeviceSelection =
        PendingOutputDeviceSelection{.backend = backend, .deviceId = deviceId, .profile = profile};
      dispatchOutward(&Impl::onStateChanged);
      return {};
    }

    optPendingOutputDeviceSelection.reset();

    auto const& device = *it;
    auto newBackendPtr = (*recordIt)->providerPtr->createBackend(device, profile);
    std::ignore = resetPlaybackGraph();

    activeBackendProvider = (*recordIt)->providerPtr.get();
    enginePtr->setBackend(std::move(newBackendPtr), device);
    acceptAudioBarrier(Engine::PreparedCancellationBarrier{.generation = enginePtr->playbackGeneration()});
    return {};
  }

  Player::Status Player::Impl::snapshot() const
  {
    auto playerStatus = Player::Status{};

    if (enginePtr)
    {
      playerStatus.engine = enginePtr->status();
    }

    {
      auto const lock = std::scoped_lock{backendsMutex};
      playerStatus.availableBackends = cachedBackends;
    }

    {
      auto const lock = std::scoped_lock{graphMutex};
      playerStatus.flow = mergedGraph;
      playerStatus.sourceQuality = qualityResult.sourceQuality;
      playerStatus.pipelineQuality = qualityResult.pipelineQuality;
      playerStatus.quality = qualityResult.overall;
      playerStatus.qualityFullyVerified = qualityResult.fullyVerified;
      playerStatus.qualityAssessments = qualityResult.assessments;
    }

    playerStatus.isReady = isReady();
    playerStatus.volume = playerStatus.engine.volume;
    playerStatus.muted = playerStatus.engine.muted;
    playerStatus.volumeAvailable = playerStatus.engine.volumeAvailable;
    playerStatus.volumeIsHardwareAssisted = playerStatus.engine.volumeIsHardwareAssisted;
    return playerStatus;
  }

  bool Player::Impl::isReady() const
  {
    return enginePtr && enginePtr->backendId() != kBackendNone && !optPendingOutputDeviceSelection;
  }

  void Player::Impl::handleRouteChanged(Engine::RouteStatus const& status, std::uint64_t generation)
  {
    if (generation != playbackGeneration.load(std::memory_order_acquire))
    {
      return;
    }

    auto lock = std::unique_lock{graphMutex};
    cachedRouteStatus = status;
    lock.unlock();

    auto const hasRoute = status.optAnchor && activeBackendProvider != nullptr;

    if (hasRoute)
    {
      if (!graphSubscription)
      {
        auto const weakSelfPtr = weak_from_this();
        auto subscription = activeBackendProvider->subscribeGraph(
          status.optAnchor->id,
          [weakSelfPtr, generation](flow::Graph const& graph)
          {
            if (auto selfPtr = weakSelfPtr.lock(); selfPtr && selfPtr->gatePtr->canAcceptCallbacks())
            {
              selfPtr->deferInternal([graph = flow::Graph{graph}, generation](Impl& impl)
                                     { impl.handleSystemGraphChanged(graph, generation); });
            }
          });

        if (!gatePtr->canAcceptCallbacks())
        {
          return;
        }

        graphSubscription = std::move(subscription);
      }
    }
    else
    {
      graphSubscription.reset();
    }

    lock.lock();

    if (!hasRoute)
    {
      cachedSystemGraph = {};
    }

    updateMergedGraph();
    lock.unlock();

    auto const playerStatus = snapshot();
    dispatchOutward(&Impl::onQualityChanged, qualityResultFromStatus(playerStatus), playerStatus.isReady);
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
          flow::Connection{.sourceId = "ao-source", .destinationId = "ao-decoder", .isActive = true},
          flow::Connection{.sourceId = "ao-decoder", .destinationId = "ao-engine", .isActive = true},
        },
    };

    for (auto const& node : cachedSystemGraph.nodes)
    {
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
      mergedGraph.connections.push_back({.sourceId = "ao-engine", .destinationId = streamNodeId, .isActive = true});
    }

    qualityResult = analyzeAudioQuality(mergedGraph);
  }

  Player::Player(async::Executor& executor)
    : Player{executor, nullptr}
  {
  }

  Player::Player(async::Executor& executor, DecoderFactoryFn decoderFactory)
    : _implPtr{std::make_shared<Impl>(executor)}
  {
    // Start with a NullBackend until a provider provides something real
    _implPtr->enginePtr = std::make_unique<Engine>(std::make_unique<NullBackend>(),
                                                   Device{.id = DeviceId{"null"},
                                                          .displayName = "None",
                                                          .description = "No audio output device selected",
                                                          .backendId = kBackendNone,
                                                          .capabilities = {}},
                                                   std::move(decoderFactory));

    _implPtr->connectEngineCallbacks();
  }

  void Player::setOnTrackEnded(std::function<void(Engine::TrackEnded const&)> callback)
  {
    auto const implPtr = _implPtr;
    implPtr->onTrackEnded = std::move(callback);
  }

  void Player::setOnTrackAdvanced(std::function<void(Engine::TrackAdvanced const&)> callback)
  {
    auto const implPtr = _implPtr;
    implPtr->onTrackAdvanced = std::move(callback);
  }

  void Player::setOnPlaybackFailure(std::function<void(Engine::PlaybackFailure const&)> callback)
  {
    auto const implPtr = _implPtr;
    implPtr->onPlaybackFailure = std::move(callback);
  }

  void Player::setOnStateChanged(std::function<void()> callback)
  {
    auto const implPtr = _implPtr;
    implPtr->onStateChanged = std::move(callback);
  }

  void Player::setOnOutputDevicesChanged(std::function<void(std::vector<BackendProvider::Status> const&)> callback)
  {
    auto const implPtr = _implPtr;
    implPtr->onOutputDevicesChanged = std::move(callback);
  }

  void Player::setOnQualityChanged(std::function<void(QualityResult const& quality, bool ready)> callback)
  {
    auto const implPtr = _implPtr;
    implPtr->onQualityChanged = std::move(callback);
  }

  Player::~Player()
  {
    shutdown();
  }

  void Player::shutdown() noexcept
  {
    if (auto const implPtr = _implPtr; implPtr)
    {
      implPtr->shutdown();
    }
  }

  void Player::addProvider(std::unique_ptr<BackendProvider> providerPtr)
  {
    if (!providerPtr)
    {
      return;
    }

    auto const implPtr = _implPtr;
    auto recordPtr = std::make_unique<Impl::ProviderRecord>();
    recordPtr->providerPtr = std::move(providerPtr);

    auto* const provider = recordPtr->providerPtr.get();
    auto* const recordRaw = recordPtr.get();
    implPtr->providers.push_back(std::move(recordPtr));

    auto const weakImplPtr = std::weak_ptr<Impl>{implPtr};
    auto subscription = provider->subscribeDevices(
      [weakImplPtr, provider](std::vector<Device> const& devices)
      {
        auto implPtr = weakImplPtr.lock();

        if (!implPtr || !implPtr->gatePtr->canAcceptCallbacks())
        {
          return false;
        }

        implPtr->dispatchInternal([provider, devices = std::vector<Device>{devices}](Impl& self)
                                  { self.handleOutputDevicesChanged(provider, devices); });
        return true;
      });

    if (implPtr->gatePtr->canAcceptCallbacks())
    {
      recordRaw->subscription = std::move(subscription);
    }
  }

  Result<> Player::play(Engine::PlaybackItem const& item, std::chrono::milliseconds const initialOffset)
  {
    auto preparedStart = stagePlayback(item, initialOffset);

    if (!preparedStart)
    {
      return std::unexpected{preparedStart.error()};
    }

    if (auto committed = commitPlayback(std::move(*preparedStart)); !committed)
    {
      return std::unexpected{committed.error()};
    }

    return {};
  }

  Result<Engine::PreparedPlaybackStart> Player::stagePlayback(Engine::PlaybackItem const& item,
                                                              std::chrono::milliseconds const initialOffset)
  {
    auto const implPtr = _implPtr;

    if (!implPtr->isReady())
    {
      // Not an internal fault: device discovery simply has not finished. Hand
      // the condition back so the caller can decide whether to report or retry.
      return makeError(
        Error::Code::InvalidState, "Playback ignored: audio backend is not ready (pending device discovery)");
    }

    return implPtr->enginePtr->stagePlayback(item, initialOffset);
  }

  Result<Engine::PlaybackStartReceipt> Player::commitPlayback(Engine::PreparedPlaybackStart&& preparedStart)
  {
    auto const implPtr = _implPtr;

    if (!implPtr->isReady())
    {
      return makeError(
        Error::Code::InvalidState, "Playback ignored: audio backend is not ready (pending device discovery)");
    }

    auto receipt = implPtr->enginePtr->commitPlayback(std::move(preparedStart));

    if (!receipt)
    {
      return std::unexpected{receipt.error()};
    }

    implPtr->acceptAudioBarrier(receipt->cancellationBarrier);
    auto const routeGeneration = implPtr->resetPlaybackGraph();

    // A backend may synchronously publish route state during Engine commit.
    // Refresh after Player accepts the receipt so an inline executor cannot
    // strand that already-applied route snapshot behind the old graph epoch.
    implPtr->handleRouteChanged(implPtr->enginePtr->routeStatus(), routeGeneration);
    return receipt;
  }

  Result<Engine::PreparedNextResult> Player::prepareNext(Engine::PlaybackItem const& item)
  {
    auto const implPtr = _implPtr;

    if (!implPtr->isReady())
    {
      return makeError(
        Error::Code::InvalidState, "Prepared playback ignored: audio backend is not ready (pending device discovery)");
    }

    return implPtr->enginePtr->setNext(item);
  }

  std::optional<Engine::PlaybackItemId> Player::clearPreparedNext()
  {
    auto const implPtr = _implPtr;
    return implPtr->enginePtr->clearNext();
  }

  Result<> Player::setOutputDevice(BackendId const& backend, DeviceId const& deviceId, ProfileId const& profile)
  {
    auto const implPtr = _implPtr;
    return implPtr->setOutputDevice(backend, deviceId, profile);
  }

  void Player::pause()
  {
    auto const implPtr = _implPtr;
    implPtr->enginePtr->pause();
  }

  void Player::resume()
  {
    auto const implPtr = _implPtr;
    implPtr->enginePtr->resume();
  }

  Engine::PreparedCancellationBarrier Player::stopWithBarrier()
  {
    auto const implPtr = _implPtr;
    auto const barrier = implPtr->enginePtr->stopWithBarrier();
    implPtr->acceptAudioBarrier(barrier);
    std::ignore = implPtr->resetPlaybackGraph();
    return barrier;
  }

  void Player::stop()
  {
    std::ignore = stopWithBarrier();
  }

  void Player::seek(std::chrono::milliseconds offset)
  {
    auto const implPtr = _implPtr;
    implPtr->enginePtr->seek(offset);
  }

  Result<> Player::setVolume(float vol)
  {
    auto const implPtr = _implPtr;
    return implPtr->enginePtr->setVolume(vol);
  }

  Result<> Player::setMuted(bool muted)
  {
    auto const implPtr = _implPtr;
    return implPtr->enginePtr->setMuted(muted);
  }

  Result<> Player::toggleMute()
  {
    auto const implPtr = _implPtr;
    auto const engineStatus = implPtr->enginePtr->status();
    return implPtr->enginePtr->setMuted(!engineStatus.muted);
  }

  Player::Status Player::status() const
  {
    auto const implPtr = _implPtr;
    return implPtr->snapshot();
  }

  Transport Player::transport() const
  {
    auto const implPtr = _implPtr;
    return implPtr->enginePtr->transport();
  }

  bool Player::isReady() const
  {
    auto const implPtr = _implPtr;
    return implPtr->isReady();
  }

  void Player::handleRouteChanged(Engine::RouteStatus const& status, std::uint64_t generation)
  {
    auto const implPtr = _implPtr;
    implPtr->handleRouteChanged(status, generation);
  }

  std::uint64_t Player::playbackGeneration() const noexcept
  {
    auto const implPtr = _implPtr;
    return implPtr->playbackGeneration.load(std::memory_order_acquire);
  }

  std::uint64_t Player::audioPlaybackGeneration() const noexcept
  {
    auto const implPtr = _implPtr;
    return implPtr->enginePtr->playbackGeneration();
  }
} // namespace ao::audio
