// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "detail/TrackPreparation.h"
#include <ao/AudioCodec.h>
#include <ao/Error.h>
#include <ao/async/Executor.h>
#include <ao/async/Runtime.h>
#include <ao/async/Task.h>
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

#include <gsl-lite/gsl-lite.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <tuple>
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

    std::unexpected<Error> preparationRejectedError()
    {
      return makeError(Error::Code::Conflict, "Playback preparation was superseded during acceptance");
    }
  } // namespace

  struct Player::Impl final
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
    struct CallbackGate final : std::enable_shared_from_this<CallbackGate>
    {
      CallbackGate(async::Executor& executor, Impl& owner)
        : executor{executor}, owner{&owner}
      {
      }

      std::atomic<bool> shuttingDown{false};
      async::Executor& executor;
      Impl* owner;

      bool canAcceptCallbacks() const noexcept { return !shuttingDown.load(std::memory_order_acquire); }

      template<typename Task>
      void dispatch(Task task)
      {
        if (!canAcceptCallbacks())
        {
          return;
        }

        auto selfPtr = shared_from_this();
        executor.dispatch(
          [selfPtr = std::move(selfPtr), task = std::move(task)] mutable
          {
            if (!selfPtr->canAcceptCallbacks())
            {
              return;
            }

            gsl_Expects(selfPtr->owner != nullptr);
            task(*selfPtr->owner);
          });
      }

      bool shutdown() noexcept
      {
        gsl_Expects(executor.isCurrent());

        if (shuttingDown.exchange(true, std::memory_order_acq_rel))
        {
          return false;
        }

        owner = nullptr;
        return true;
      }
    };

    struct OutwardPublicationState final
    {
      std::atomic_size_t depth{0};
    };

    struct [[nodiscard]] OutwardPublicationScope final
    {
      explicit OutwardPublicationScope(std::shared_ptr<OutwardPublicationState> statePtr)
        : statePtr{std::move(statePtr)}
      {
        this->statePtr->depth.fetch_add(1, std::memory_order_acq_rel);
      }

      ~OutwardPublicationScope() { statePtr->depth.fetch_sub(1, std::memory_order_acq_rel); }

      OutwardPublicationScope(OutwardPublicationScope const&) = delete;
      OutwardPublicationScope& operator=(OutwardPublicationScope const&) = delete;
      OutwardPublicationScope(OutwardPublicationScope&&) = delete;
      OutwardPublicationScope& operator=(OutwardPublicationScope&&) = delete;

      std::shared_ptr<OutwardPublicationState> statePtr;
    };

    explicit Impl(async::Executor& exec, async::Runtime* runtime)
      : executor{exec}
      , asyncRuntime{runtime}
      , outwardPublicationStatePtr{std::make_shared<OutwardPublicationState>()}
      , gatePtr{std::make_shared<CallbackGate>(exec, *this)}
    {
    }

    Impl(Impl const&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl const&) = delete;
    Impl& operator=(Impl&&) = delete;

    ~Impl()
    {
      shutdown();
      gsl_Expects(outwardPublicationStatePtr->depth.load(std::memory_order_acquire) == 0);
    }

    void shutdown() noexcept
    {
      gsl_Expects(executor.isCurrent());
      gsl_Expects(outwardPublicationStatePtr->depth.load(std::memory_order_acquire) == 0);

      if (!gatePtr->shutdown())
      {
        return;
      }

      cancelStartPreparation();
      cancelLookaheadPreparation();

      // Teardown order:
      //   1. Unsubscribe graph and device callbacks so no new callbacks fire.
      //   2. Shut down providers' async activity (monitor threads, PW event loops) so no
      //      in-flight callbacks can race with engine/backend destruction.
      //   3. Stop the engine, which closes/destroys the active backend while
      //      provider-owned state (e.g. AlsaGraphRegistry) is still alive.
      //   4. Release the unique Engine and providers only after their producers
      //      have stopped and all subscription callbacks have quiesced.
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
    }

    async::Executor& executor;
    async::Runtime* asyncRuntime = nullptr;
    async::TaskHandle startPreparationTask;
    async::TaskHandle lookaheadPreparationTask;
    std::atomic<std::uint64_t> playbackGeneration{1};
    std::atomic<std::uint64_t> audioCallbackGenerationFloor{1};
    std::shared_ptr<OutwardPublicationState> outwardPublicationStatePtr;
    std::vector<std::unique_ptr<ProviderRecord>> providers;
    std::optional<PendingOutputDeviceSelection> optPendingOutputDeviceSelection;
    BackendProvider* activeBackendProvider = nullptr;
    Subscription graphSubscription;
    std::unique_ptr<Engine> enginePtr;
    std::shared_ptr<CallbackGate> gatePtr;

    mutable std::mutex backendsMutex;
    mutable std::vector<BackendProvider::Status> cachedBackends;
    mutable std::vector<Device> allDevices;

    mutable std::mutex graphMutex;
    Engine::RouteStatus cachedRouteStatus;
    flow::Graph cachedSystemGraph;
    flow::Graph mergedGraph;
    QualityResult qualityResult;
    mutable std::mutex callbacksMutex;
    std::function<void(Engine::TrackEnded const&)> onTrackEnded;
    std::function<void(Engine::TrackAdvanced const&)> onTrackAdvanced;
    std::function<void(Engine::PlaybackFailure const&)> onPlaybackFailure;
    std::function<void()> onStateChanged;
    std::function<void(std::vector<BackendProvider::Status> const&)> onOutputDevicesChanged;
    std::function<void(QualityResult const&, bool)> onQualityChanged;

    void connectEngineCallbacks() const;
    void initializeNullEngine(DecoderFactoryFn decoderFactory);
    void connectTrackEndedCallback() const;
    void connectTrackAdvancedCallback() const;
    void connectPlaybackFailureCallback() const;
    void connectStateChangedCallback() const;
    void connectRouteChangedCallback() const;
    void handleOutputDevicesChanged(BackendProvider* provider, std::vector<Device> const& devices);
    void handleSystemGraphChanged(flow::Graph const& graph, std::uint64_t generation);
    void handleRouteChanged(Engine::RouteStatus const& status, std::uint64_t generation);
    Result<> setOutputDevice(BackendId const& backend, DeviceId const& deviceId, ProfileId const& profile);
    Player::Status snapshot() const;
    bool isReady() const;
    void updateMergedGraph();

    void cancelStartPreparation() { startPreparationTask.reset(); }

    void cancelLookaheadPreparation() { lookaheadPreparationTask.reset(); }

    static async::Task<void> runStartPreparation(async::Runtime* runtime,
                                                 std::shared_ptr<CallbackGate> callbackGatePtr,
                                                 detail::TrackPreparation preparation,
                                                 Player::PreparationAcceptance acceptance,
                                                 Player::PreparedStartCompletion completion,
                                                 std::stop_token stopToken)
    {
      auto const prepared = preparation.prepare();
      co_await runtime->resumeOnCallbackExecutor(stopToken);

      if (!callbackGatePtr->canAcceptCallbacks())
      {
        co_return;
      }

      auto* owner = callbackGatePtr->owner;
      gsl_Expects(owner != nullptr);

      // Retire Player's registration before either outward callback. A
      // reentrant replacement may then install its own task without being
      // cleared by this completion path.
      owner->startPreparationTask = {};
      auto publicationStatePtr = owner->outwardPublicationStatePtr;
      bool accepted = false;

      {
        auto publication = OutwardPublicationScope{publicationStatePtr};
        accepted = acceptance();
      }

      if (!callbackGatePtr->canAcceptCallbacks())
      {
        co_return;
      }

      owner = callbackGatePtr->owner;
      gsl_Expects(owner != nullptr);
      auto outcome = Result<Engine::PreparedPlaybackStart>{preparationRejectedError()};

      if (accepted)
      {
        if (!prepared)
        {
          outcome = std::unexpected{prepared.error()};
        }
        else
        {
          outcome = std::move(preparation).adoptStart(*owner->enginePtr);
        }
      }

      publicationStatePtr = owner->outwardPublicationStatePtr;
      auto publication = OutwardPublicationScope{publicationStatePtr};
      completion(std::move(outcome));
    }

    static async::Task<void> runLookaheadPreparation(async::Runtime* runtime,
                                                     std::shared_ptr<CallbackGate> callbackGatePtr,
                                                     detail::TrackPreparation preparation,
                                                     Player::PreparationAcceptance acceptance,
                                                     Player::PreparedNextCompletion completion,
                                                     std::stop_token stopToken)
    {
      auto const prepared = preparation.prepare();
      co_await runtime->resumeOnCallbackExecutor(stopToken);

      if (!callbackGatePtr->canAcceptCallbacks())
      {
        co_return;
      }

      auto* owner = callbackGatePtr->owner;
      gsl_Expects(owner != nullptr);

      // Retire Player's registration before either outward callback. A
      // reentrant replacement may then install its own task without being
      // cleared by this completion path.
      owner->lookaheadPreparationTask = {};
      auto publicationStatePtr = owner->outwardPublicationStatePtr;
      bool accepted = false;

      {
        auto publication = OutwardPublicationScope{publicationStatePtr};
        accepted = acceptance();
      }

      if (!callbackGatePtr->canAcceptCallbacks())
      {
        co_return;
      }

      owner = callbackGatePtr->owner;
      gsl_Expects(owner != nullptr);
      auto outcome = Result<Engine::PreparedNextResult>{preparationRejectedError()};

      if (accepted)
      {
        if (!prepared)
        {
          outcome = std::unexpected{prepared.error()};
        }
        else
        {
          outcome = std::move(preparation).adoptNext(*owner->enginePtr);
        }
      }

      publicationStatePtr = owner->outwardPublicationStatePtr;
      auto publication = OutwardPublicationScope{publicationStatePtr};
      completion(std::move(outcome));
    }

    void ensureOnExecutor() const noexcept { gsl_Expects(executor.isCurrent()); }

    template<typename Slot>
    Slot copyOutwardCallback(Slot Impl::* slot) const
    {
      auto const lock = std::scoped_lock{callbacksMutex};
      return this->*slot;
    }

    template<typename Slot, typename... Args>
    void invokeOutward(Slot Impl::* slot, Args&&... args)
    {
      auto callback = copyOutwardCallback(slot);

      if (!callback)
      {
        return;
      }

      auto publication = OutwardPublicationScope{outwardPublicationStatePtr};
      std::invoke(callback, std::forward<Args>(args)...);
    }

    template<typename Slot>
    void setOutwardCallback(Slot Impl::* slot, Slot callback)
    {
      {
        auto const lock = std::scoped_lock{callbacksMutex};
        std::swap(this->*slot, callback);
      }
      // Destroy the replaced callback after unlocking; captured objects may
      // have teardown behavior that must not re-enter while the mutex is held.
    }

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

    // Provider callbacks can be synchronous backend-control callbacks. Hand
    // graph work through Engine's event queue first so even an inline executor
    // cannot run Player/user callbacks while Engine holds its control lock.
    template<typename Task>
    void deferInternal(Task task)
    {
      auto callbackGatePtr = gatePtr;
      enginePtr->defer([callbackGatePtr = std::move(callbackGatePtr), task = std::move(task)] mutable
                       { callbackGatePtr->dispatch(std::move(task)); });
    }

    // Marshal one of the outward on* callback slots onto the executor thread.
    // The task copies the user callback while the gate is open, then invokes the
    // copy without holding any teardown wait state. Queued tasks become no-ops
    // after teardown; synchronous owner destruction is rejected by contract.
    template<typename Slot, typename... Args>
    void dispatchOutward(Slot Impl::* slot, Args... args)
    {
      gatePtr->dispatch(
        [slot, args = std::make_tuple(std::move(args)...)](Impl& self) mutable
        {
          std::apply([&self, slot](auto&&... unpacked)
                     { self.invokeOutward(slot, std::forward<decltype(unpacked)>(unpacked)...); },
                     std::move(args));
        });
    }
  };

  void Player::Impl::connectEngineCallbacks() const
  {
    connectTrackEndedCallback();
    connectTrackAdvancedCallback();
    connectPlaybackFailureCallback();
    connectStateChangedCallback();
    connectRouteChangedCallback();
  }

  void Player::Impl::initializeNullEngine(DecoderFactoryFn decoderFactory)
  {
    ensureOnExecutor();
    // Start with a NullBackend until a provider provides something real
    enginePtr = std::make_unique<Engine>(std::make_unique<NullBackend>(),
                                         Device{.id = DeviceId{"null"},
                                                .displayName = "None",
                                                .description = "No audio output device selected",
                                                .backendId = kBackendNone},
                                         std::move(decoderFactory));
    connectEngineCallbacks();
  }

  void Player::Impl::connectTrackEndedCallback() const
  {
    auto const callbackGatePtr = gatePtr;
    enginePtr->setOnTrackEnded(
      [callbackGatePtr](Engine::TrackEnded const& event)
      {
        callbackGatePtr->dispatch(
          [event](Impl& self)
          {
            if (!self.acceptsAudioCallback(event.generation))
            {
              return;
            }

            self.invokeOutward(&Impl::onTrackEnded, event);
          });
      });
  }

  void Player::Impl::connectTrackAdvancedCallback() const
  {
    auto const callbackGatePtr = gatePtr;
    enginePtr->setOnTrackAdvanced(
      [callbackGatePtr](Engine::TrackAdvanced const& event)
      {
        callbackGatePtr->dispatch(
          [event = Engine::TrackAdvanced{event}](Impl& self)
          {
            if (!self.acceptsAudioCallback(event.generation))
            {
              return;
            }

            std::ignore = self.resetPlaybackGraph();

            self.invokeOutward(&Impl::onTrackAdvanced, event);
          });
      });
  }

  void Player::Impl::connectPlaybackFailureCallback() const
  {
    auto const callbackGatePtr = gatePtr;
    enginePtr->setOnPlaybackFailure(
      [callbackGatePtr](Engine::PlaybackFailure const& failure)
      {
        callbackGatePtr->dispatch(
          [failure = Engine::PlaybackFailure{failure}](Impl& self)
          {
            if (!self.acceptsAudioCallback(failure.generation))
            {
              return;
            }

            self.invokeOutward(&Impl::onPlaybackFailure, failure);
          });
      });
  }

  void Player::Impl::connectStateChangedCallback() const
  {
    auto const callbackGatePtr = gatePtr;
    enginePtr->setOnStateChanged(
      [callbackGatePtr] { callbackGatePtr->dispatch([](Impl& self) { self.invokeOutward(&Impl::onStateChanged); }); });
  }

  void Player::Impl::connectRouteChangedCallback() const
  {
    auto const callbackGatePtr = gatePtr;
    enginePtr->setOnRouteChanged(
      [callbackGatePtr](Engine::RouteStatus const& status)
      {
        callbackGatePtr->dispatch(
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
        auto const callbackGatePtr = gatePtr;
        auto subscription = activeBackendProvider->subscribeGraph(
          status.optAnchor->id,
          [callbackGatePtr, generation](flow::Graph const& graph)
          {
            callbackGatePtr->dispatch(
              [graph = flow::Graph{graph}, generation](Impl& self) mutable
              {
                self.deferInternal([graph = std::move(graph), generation](Impl& owner)
                                   { owner.handleSystemGraphChanged(graph, generation); });
              });
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
    : _implPtr{std::make_unique<Impl>(executor, nullptr)}
  {
    _implPtr->initializeNullEngine(std::move(decoderFactory));
  }

  Player::Player(async::Runtime& runtime)
    : Player{runtime, nullptr}
  {
  }

  Player::Player(async::Runtime& runtime, DecoderFactoryFn decoderFactory)
    : _implPtr{std::make_unique<Impl>(runtime.callbackExecutor(), &runtime)}
  {
    _implPtr->initializeNullEngine(std::move(decoderFactory));
  }

  void Player::setOnTrackEnded(std::function<void(Engine::TrackEnded const&)> callback)
  {
    _implPtr->ensureOnExecutor();
    _implPtr->setOutwardCallback(&Impl::onTrackEnded, std::move(callback));
  }

  void Player::setOnTrackAdvanced(std::function<void(Engine::TrackAdvanced const&)> callback)
  {
    _implPtr->ensureOnExecutor();
    _implPtr->setOutwardCallback(&Impl::onTrackAdvanced, std::move(callback));
  }

  void Player::setOnPlaybackFailure(std::function<void(Engine::PlaybackFailure const&)> callback)
  {
    _implPtr->ensureOnExecutor();
    _implPtr->setOutwardCallback(&Impl::onPlaybackFailure, std::move(callback));
  }

  void Player::setOnStateChanged(std::function<void()> callback)
  {
    _implPtr->ensureOnExecutor();
    _implPtr->setOutwardCallback(&Impl::onStateChanged, std::move(callback));
  }

  void Player::setOnOutputDevicesChanged(std::function<void(std::vector<BackendProvider::Status> const&)> callback)
  {
    _implPtr->ensureOnExecutor();
    _implPtr->setOutwardCallback(&Impl::onOutputDevicesChanged, std::move(callback));
  }

  void Player::setOnQualityChanged(std::function<void(QualityResult const& quality, bool ready)> callback)
  {
    _implPtr->ensureOnExecutor();
    _implPtr->setOutwardCallback(&Impl::onQualityChanged, std::move(callback));
  }

  Player::~Player()
  {
    gsl_Expects(_implPtr != nullptr);
    _implPtr->ensureOnExecutor();
    gsl_Expects(_implPtr->outwardPublicationStatePtr->depth.load(std::memory_order_acquire) == 0);
    shutdown();
  }

  void Player::shutdown() noexcept
  {
    _implPtr->ensureOnExecutor();
    _implPtr->shutdown();
  }

  void Player::addProvider(std::unique_ptr<BackendProvider> providerPtr)
  {
    _implPtr->ensureOnExecutor();

    if (!providerPtr)
    {
      return;
    }

    auto recordPtr = std::make_unique<Impl::ProviderRecord>();
    recordPtr->providerPtr = std::move(providerPtr);

    auto* const provider = recordPtr->providerPtr.get();
    auto* const recordRaw = recordPtr.get();
    _implPtr->providers.push_back(std::move(recordPtr));

    auto const callbackGatePtr = _implPtr->gatePtr;
    auto subscription = provider->subscribeDevices(
      [callbackGatePtr, provider](std::vector<Device> const& devices)
      {
        if (!callbackGatePtr->canAcceptCallbacks())
        {
          return false;
        }

        callbackGatePtr->dispatch([provider, devices = std::vector<Device>{devices}](Impl& self)
                                  { self.handleOutputDevicesChanged(provider, devices); });
        return true;
      });

    if (_implPtr->gatePtr->canAcceptCallbacks())
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
    _implPtr->ensureOnExecutor();
    _implPtr->cancelStartPreparation();
    _implPtr->cancelLookaheadPreparation();

    if (!_implPtr->isReady())
    {
      // Not an internal fault: device discovery simply has not finished. Hand
      // the condition back so the caller can decide whether to report or retry.
      return makeError(
        Error::Code::InvalidState, "Playback ignored: audio backend is not ready (pending device discovery)");
    }

    return _implPtr->enginePtr->stagePlayback(item, initialOffset);
  }

  Result<> Player::stagePlaybackAsync(Engine::PlaybackItem const& item,
                                      std::chrono::milliseconds const initialOffset,
                                      PreparationAcceptance acceptance,
                                      PreparedStartCompletion completion)
  {
    _implPtr->ensureOnExecutor();
    _implPtr->cancelStartPreparation();
    _implPtr->cancelLookaheadPreparation();

    if (_implPtr->asyncRuntime == nullptr)
    {
      return makeError(Error::Code::InvalidState, "Asynchronous playback preparation requires an async runtime");
    }

    if (!_implPtr->isReady())
    {
      return makeError(
        Error::Code::InvalidState, "Playback ignored: audio backend is not ready (pending device discovery)");
    }

    auto preparation = detail::TrackPreparation::capture(
      *_implPtr->enginePtr, item, initialOffset, detail::TrackPreparation::Purpose::ExplicitStart);

    if (!preparation)
    {
      return std::unexpected{preparation.error()};
    }

    auto* const runtime = _implPtr->asyncRuntime;
    auto const callbackGatePtr = _implPtr->gatePtr;
    _implPtr->startPreparationTask = runtime->spawnCancellable(
      [runtime,
       callbackGatePtr,
       preparation = std::move(*preparation),
       acceptance = std::move(acceptance),
       completion = std::move(completion)](std::stop_token const stopToken) mutable
      {
        return Impl::runStartPreparation(runtime,
                                         std::move(callbackGatePtr),
                                         std::move(preparation),
                                         std::move(acceptance),
                                         std::move(completion),
                                         stopToken);
      });
    return {};
  }

  Result<Engine::PlaybackStartReceipt> Player::commitPlayback(Engine::PreparedPlaybackStart&& preparedStart)
  {
    _implPtr->ensureOnExecutor();

    if (!_implPtr->isReady())
    {
      return makeError(
        Error::Code::InvalidState, "Playback ignored: audio backend is not ready (pending device discovery)");
    }

    auto receipt = _implPtr->enginePtr->commitPlayback(std::move(preparedStart));

    if (!receipt)
    {
      return std::unexpected{receipt.error()};
    }

    _implPtr->acceptAudioBarrier(receipt->cancellationBarrier);
    auto const routeGeneration = _implPtr->resetPlaybackGraph();

    // A backend may synchronously publish route state during Engine commit.
    // Refresh after Player accepts the receipt so an inline executor cannot
    // strand that already-applied route snapshot behind the old graph epoch.
    _implPtr->handleRouteChanged(_implPtr->enginePtr->routeStatus(), routeGeneration);
    return receipt;
  }

  Result<Engine::PreparedNextResult> Player::prepareNext(Engine::PlaybackItem const& item)
  {
    _implPtr->ensureOnExecutor();
    _implPtr->cancelLookaheadPreparation();

    if (!_implPtr->isReady())
    {
      return makeError(
        Error::Code::InvalidState, "Prepared playback ignored: audio backend is not ready (pending device discovery)");
    }

    return _implPtr->enginePtr->setNext(item);
  }

  Result<> Player::prepareNextAsync(Engine::PlaybackItem const& item,
                                    PreparationAcceptance acceptance,
                                    PreparedNextCompletion completion)
  {
    _implPtr->ensureOnExecutor();
    _implPtr->cancelLookaheadPreparation();

    if (_implPtr->asyncRuntime == nullptr)
    {
      return makeError(Error::Code::InvalidState, "Asynchronous lookahead preparation requires an async runtime");
    }

    if (!_implPtr->isReady())
    {
      return makeError(
        Error::Code::InvalidState, "Prepared playback ignored: audio backend is not ready (pending device discovery)");
    }

    auto preparation = detail::TrackPreparation::capture(
      *_implPtr->enginePtr, item, {}, detail::TrackPreparation::Purpose::GaplessLookahead);

    if (!preparation)
    {
      return std::unexpected{preparation.error()};
    }

    if (!preparation->requiresWorker())
    {
      auto const prepared = preparation->prepare();
      auto const callbackGatePtr = _implPtr->gatePtr;
      auto publicationStatePtr = _implPtr->outwardPublicationStatePtr;
      bool accepted = false;

      {
        auto publication = Impl::OutwardPublicationScope{publicationStatePtr};
        accepted = acceptance();
      }

      if (!callbackGatePtr->canAcceptCallbacks())
      {
        return {};
      }

      auto* const owner = callbackGatePtr->owner;
      gsl_Expects(owner != nullptr);
      auto outcome = Result<Engine::PreparedNextResult>{preparationRejectedError()};

      if (accepted)
      {
        if (!prepared)
        {
          outcome = std::unexpected{prepared.error()};
        }
        else
        {
          outcome = std::move(*preparation).adoptNext(*owner->enginePtr);
        }
      }

      publicationStatePtr = owner->outwardPublicationStatePtr;

      {
        auto publication = Impl::OutwardPublicationScope{publicationStatePtr};
        completion(std::move(outcome));
      }

      return {};
    }

    auto* const runtime = _implPtr->asyncRuntime;
    auto const callbackGatePtr = _implPtr->gatePtr;
    _implPtr->lookaheadPreparationTask = runtime->spawnCancellable(
      [runtime,
       callbackGatePtr,
       preparation = std::move(*preparation),
       acceptance = std::move(acceptance),
       completion = std::move(completion)](std::stop_token const stopToken) mutable
      {
        return Impl::runLookaheadPreparation(runtime,
                                             std::move(callbackGatePtr),
                                             std::move(preparation),
                                             std::move(acceptance),
                                             std::move(completion),
                                             stopToken);
      });
    return {};
  }

  void Player::cancelStartPreparation()
  {
    _implPtr->ensureOnExecutor();
    _implPtr->cancelStartPreparation();
  }

  void Player::cancelLookaheadPreparation()
  {
    _implPtr->ensureOnExecutor();
    _implPtr->cancelLookaheadPreparation();
  }

  std::optional<Engine::PlaybackItemId> Player::clearPreparedNext()
  {
    _implPtr->ensureOnExecutor();
    _implPtr->cancelLookaheadPreparation();
    return _implPtr->enginePtr->clearNext();
  }

  Result<> Player::setOutputDevice(BackendId const& backend, DeviceId const& deviceId, ProfileId const& profile)
  {
    _implPtr->ensureOnExecutor();
    _implPtr->cancelStartPreparation();
    _implPtr->cancelLookaheadPreparation();
    return _implPtr->setOutputDevice(backend, deviceId, profile);
  }

  void Player::pause()
  {
    _implPtr->ensureOnExecutor();
    _implPtr->enginePtr->pause();
  }

  void Player::resume()
  {
    _implPtr->ensureOnExecutor();
    _implPtr->enginePtr->resume();
  }

  Engine::PreparedCancellationBarrier Player::stopWithBarrier()
  {
    _implPtr->ensureOnExecutor();
    _implPtr->cancelStartPreparation();
    _implPtr->cancelLookaheadPreparation();
    auto const barrier = _implPtr->enginePtr->stopWithBarrier();
    _implPtr->acceptAudioBarrier(barrier);
    std::ignore = _implPtr->resetPlaybackGraph();
    return barrier;
  }

  void Player::stop()
  {
    std::ignore = stopWithBarrier();
  }

  void Player::seek(std::chrono::milliseconds offset)
  {
    _implPtr->ensureOnExecutor();
    _implPtr->cancelStartPreparation();
    _implPtr->cancelLookaheadPreparation();
    _implPtr->enginePtr->seek(offset);
  }

  Result<> Player::setVolume(float vol)
  {
    _implPtr->ensureOnExecutor();
    return _implPtr->enginePtr->setVolume(vol);
  }

  Result<> Player::setMuted(bool muted)
  {
    _implPtr->ensureOnExecutor();
    return _implPtr->enginePtr->setMuted(muted);
  }

  Result<> Player::toggleMute()
  {
    _implPtr->ensureOnExecutor();
    auto const engineStatus = _implPtr->enginePtr->status();
    return _implPtr->enginePtr->setMuted(!engineStatus.muted);
  }

  Player::Status Player::status() const
  {
    _implPtr->ensureOnExecutor();
    return _implPtr->snapshot();
  }

  Transport Player::transport() const
  {
    _implPtr->ensureOnExecutor();
    return _implPtr->enginePtr->transport();
  }

  bool Player::isReady() const
  {
    _implPtr->ensureOnExecutor();
    return _implPtr->isReady();
  }

  void Player::handleRouteChanged(Engine::RouteStatus const& status, std::uint64_t generation)
  {
    _implPtr->ensureOnExecutor();
    _implPtr->handleRouteChanged(status, generation);
  }

  std::uint64_t Player::playbackGeneration() const noexcept
  {
    _implPtr->ensureOnExecutor();
    return _implPtr->playbackGeneration.load(std::memory_order_acquire);
  }

  std::uint64_t Player::audioPlaybackGeneration() const noexcept
  {
    _implPtr->ensureOnExecutor();
    return _implPtr->enginePtr->playbackGeneration();
  }
} // namespace ao::audio
