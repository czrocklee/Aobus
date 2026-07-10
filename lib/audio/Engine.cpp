// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "detail/RenderPath.h"
#include "detail/RenderTimeline.h"
#include "detail/TrackSession.h"
#include <ao/AudioCodec.h>
#include <ao/Error.h>
#include <ao/audio/Backend.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/DecodedStreamInfo.h>
#include <ao/audio/Device.h>
#include <ao/audio/Engine.h>
#include <ao/audio/Format.h>
#include <ao/audio/PcmSource.h>
#include <ao/audio/Property.h>
#include <ao/audio/RenderTarget.h>
#include <ao/audio/Transport.h>
#include <ao/audio/detail/RouteTracker.h>

#include <boost/lockfree/policies.hpp>
#include <boost/lockfree/spsc_queue.hpp>

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <semaphore>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace ao::audio
{
  namespace
  {
    template<typename Action>
    void terminateOnException(Action&& action) noexcept
    {
      try
      {
        std::forward<Action>(action)();
      }
      catch (...)
      {
        std::terminate();
      }
    }
  } // namespace

  // ── Engine::Impl: data bucket + callbacks + handlers ────────────

  struct Engine::Impl final
  {
    struct EngineRenderTarget;
    using RenderTimeline = detail::RenderTimeline;
    using TrackNode = RenderTimeline::Node;
    struct BackendErrorEvent;
    struct SourceErrorEvent;
    struct DrainCompleteEvent;
    struct RouteReadyEvent;
    struct FormatChangedEvent;
    struct PropertyChangedEvent;

    struct BackendErrorEvent final
    {
      std::uint64_t generation = 0;
      std::string message;
    };

    struct SourceErrorEvent final
    {
      std::uint64_t sourceGeneration = 0;
      Error error;
    };

    // Wait-free render-thread -> event-thread notification. Posted by the RT
    // render callback at a gapless boundary (Spliced) or when the current source
    // finishes with no gapless successor (Drained). Spliced carries a non-owning
    // pointer to the Engine-owned lookahead node; the consumer promotes that
    // node to current before publishing callbacks.
    enum class RtSignalKind : std::uint8_t
    {
      Spliced,
      Drained,
    };

    struct RtSignal final
    {
      RtSignalKind kind = RtSignalKind::Drained;
      std::uint64_t generation = 0;
      TrackNode* splicedNode = nullptr;
      std::uint64_t drainEpoch = 0;
    };

    struct DrainCompleteEvent final
    {
      std::uint64_t generation = 0;
      std::uint64_t drainEpoch = 0;
    };

    struct RouteReadyEvent final
    {
      std::uint64_t generation = 0;
      BackendId backendId;
      std::string routeAnchor;
    };

    struct FormatChangedEvent final
    {
      std::uint64_t generation = 0;
      Format format;
    };

    struct PropertyChangedEvent final
    {
      std::uint64_t generation = 0;
      PropertySnapshot snapshot;
    };

    // Notifications already materialized by a control thread that settled a
    // splice signal at command entry (see lockControl). The event worker just
    // runs them, so user callbacks keep originating from the worker thread in
    // application order.
    struct DeferredNotifications final
    {
      std::vector<std::function<void()>> notifications;
    };

    using PlaybackEvent = std::variant<BackendErrorEvent,
                                       SourceErrorEvent,
                                       DrainCompleteEvent,
                                       RouteReadyEvent,
                                       FormatChangedEvent,
                                       PropertyChangedEvent,
                                       DeferredNotifications>;
    using Notification = std::function<void()>;
    using Notifications = std::vector<Notification>;

    struct PendingRouteNotifications final
    {
      std::function<void()> stateChanged;
      OnRouteChanged routeChanged;
      RouteStatus routeSnapshot;
    };

    class EngineEventQueue final
    {
    public:
      using RtSignalProcessor = std::function<std::optional<Notifications>()>;
      using PlaybackEventProcessor = std::function<Notifications(PlaybackEvent&)>;

      EngineEventQueue() = default;

      ~EngineEventQueue()
      {
        assert(!_eventThread.joinable() && "EngineEventQueue owner must stop the queue before destruction");
        assert(!_running.load(std::memory_order_acquire) &&
               "EngineEventQueue worker must exit before queue destruction");
        assert(_playbackEvents.empty() && "EngineEventQueue owner must clear playback events before destruction");
        assert(_rtSignalRing.read_available() == 0 &&
               "EngineEventQueue owner must drain RT signals before destruction");
      }

      EngineEventQueue(EngineEventQueue const&) = delete;
      EngineEventQueue& operator=(EngineEventQueue const&) = delete;
      EngineEventQueue(EngineEventQueue&&) = delete;
      EngineEventQueue& operator=(EngineEventQueue&&) = delete;

      void start(RtSignalProcessor processNextRtSignal, PlaybackEventProcessor processPlaybackEvent)
      {
        _running.store(true, std::memory_order_release);

        try
        {
          _eventThread =
            std::jthread{[this,
                          processNextRtSignal = std::move(processNextRtSignal),
                          processPlaybackEvent = std::move(processPlaybackEvent)](std::stop_token stopToken) mutable
                         {
                           run(stopToken, processNextRtSignal, processPlaybackEvent);
                           _running.store(false, std::memory_order_release);
                           _running.notify_all();
                         }};
        }
        catch (...)
        {
          _running.store(false, std::memory_order_release);
          _running.notify_all();
          throw;
        }
      }

      bool isCurrentThread() const noexcept { return _currentQueue == this; }

      void waitForExit() const noexcept
      {
        if (isCurrentThread())
        {
          return;
        }

        while (_running.load(std::memory_order_acquire))
        {
          _running.wait(true, std::memory_order_acquire);
        }
      }

      void enqueuePlaybackEvent(PlaybackEvent event)
      {
        if (_eventThread.get_stop_token().stop_requested())
        {
          return;
        }

        {
          auto const lock = std::scoped_lock{_playbackEventMutex};

          if (_eventThread.get_stop_token().stop_requested())
          {
            return;
          }

          _playbackEvents.push_back(std::move(event));
        }

        _eventSignal.release();
      }

      bool enqueueRtSignal(RtSignal signal) noexcept
      {
        if (!_rtSignalRing.push(signal))
        {
          assert(false && "RT signal ring overflow: event thread is not draining");
          return false;
        }

        _eventSignal.release();
        return true;
      }

      // Consumer-side only. Runtime consumers serialize pop+processing outside
      // this class because control commands also settle this ring.
      bool tryPopRtSignal(RtSignal& signal) { return _rtSignalRing.pop(signal); }

      template<typename RtSignalConsumer>
      void drainRtSignals(RtSignalConsumer consume)
      {
        auto signal = RtSignal{};

        while (tryPopRtSignal(signal))
        {
          consume(signal);
        }
      }

      template<typename RtSignalConsumer>
      void stop(RtSignalConsumer discardSignal) noexcept
      {
        if (_eventThread.joinable())
        {
          _eventThread.request_stop();
          _eventSignal.release();

          if (isCurrentThread())
          {
            // The worker's callbacks keep Engine::Impl alive until run()
            // returns. Detaching here avoids joining the current thread; the
            // stop checks below prevent another callback from observing an
            // owner that reentrantly destroyed Engine/Player.
            _eventThread.detach();
          }
          else
          {
            _eventThread.join();
          }
        }

        {
          auto const lock = std::scoped_lock{_playbackEventMutex};
          _playbackEvents.clear();
        }

        drainRtSignals(discardSignal);
      }

    private:
      static constexpr std::size_t kRtSignalCapacity = 64;

      mutable std::mutex _playbackEventMutex;
      std::deque<PlaybackEvent> _playbackEvents;
      boost::lockfree::spsc_queue<RtSignal, boost::lockfree::capacity<kRtSignalCapacity>> _rtSignalRing;
      std::counting_semaphore<> _eventSignal{0};
      std::jthread _eventThread;
      std::atomic<bool> _running{false};

      class [[nodiscard]] CurrentQueueGuard final
      {
      public:
        explicit CurrentQueueGuard(EngineEventQueue& queue)
          : _previous{std::exchange(_currentQueue, &queue)}
        {
        }

        ~CurrentQueueGuard() { _currentQueue = _previous; }

        CurrentQueueGuard(CurrentQueueGuard const&) = delete;
        CurrentQueueGuard& operator=(CurrentQueueGuard const&) = delete;
        CurrentQueueGuard(CurrentQueueGuard&&) = delete;
        CurrentQueueGuard& operator=(CurrentQueueGuard&&) = delete;

      private:
        EngineEventQueue* _previous;
      };

      static bool runNotifications(std::stop_token const& stopToken, Notifications& notifications)
      {
        for (auto& notification : notifications)
        {
          if (stopToken.stop_requested())
          {
            return false;
          }

          if (notification)
          {
            notification();
          }
        }

        return !stopToken.stop_requested();
      }

      void run(std::stop_token stopToken,
               RtSignalProcessor& processNextRtSignal,
               PlaybackEventProcessor& processPlaybackEvent)
      {
        auto const currentQueueGuard = CurrentQueueGuard{*this};

        while (true)
        {
          _eventSignal.acquire();

          if (stopToken.stop_requested())
          {
            return;
          }

          // Drain the wait-free render-thread ring first so a gapless advance
          // is published before any queued non-RT events. The owner pops and
          // processes the signal in one externally serialized critical section.
          while (true)
          {
            if (stopToken.stop_requested())
            {
              return;
            }

            auto optNotifications = processNextRtSignal();

            if (!optNotifications)
            {
              break;
            }

            if (!runNotifications(stopToken, *optNotifications))
            {
              return;
            }
          }

          while (true)
          {
            if (stopToken.stop_requested())
            {
              return;
            }

            auto optEvent = std::optional<PlaybackEvent>{};
            {
              auto const lock = std::scoped_lock{_playbackEventMutex};

              if (_playbackEvents.empty())
              {
                break;
              }

              optEvent = std::move(_playbackEvents.front());
              _playbackEvents.pop_front();
            }

            if (auto notifications = processPlaybackEvent(*optEvent); !runNotifications(stopToken, notifications))
            {
              return;
            }
          }
        }
      }

      inline static thread_local EngineEventQueue* _currentQueue = nullptr;
    };

    Device currentDevice;
    RenderTimeline timeline;

    // Serializes external control commands that coordinate backend lifecycle,
    // source ownership, and status publication. Backend callbacks deliberately
    // do not take this lock; they may run on the render thread while stop()
    // waits for that thread to quiesce.
    mutable std::mutex controlMutex;
    std::condition_variable shutdownCv;

    enum class LifecycleState : std::uint8_t
    {
      Running,
      ShuttingDown,
      Shutdown,
    };

    LifecycleState lifecycleState = LifecycleState::Running;

    class [[nodiscard]] ControlLock final
    {
    public:
      explicit ControlLock(Impl& owner)
        : _lock{owner.controlMutex}
      {
        if (owner.lifecycleState != LifecycleState::Running)
        {
          return;
        }

        owner.waitForSpliceHandoff();
        owner.settlePendingRtSignals();
        _active = true;
      }

      ~ControlLock();

      explicit operator bool() const noexcept { return _active; }

      ControlLock(ControlLock const&) = delete;
      ControlLock& operator=(ControlLock const&) = delete;
      ControlLock(ControlLock&&) = delete;
      ControlLock& operator=(ControlLock&&) = delete;

    private:
      std::unique_lock<std::mutex> _lock;
      bool _active = false;
    };

    std::atomic<bool> backendStarted{false};
    std::atomic<bool> playbackDrainPending{false};
    std::atomic<std::uint64_t> drainEpoch{1};
    std::atomic<std::uint32_t> underrunCount{0};
    std::atomic<std::uint64_t> accumulatedFrames{0};
    std::atomic<std::uint32_t> engineSampleRate{0};
    std::atomic<std::uint32_t> engineFrameBytes{0};
    std::atomic<std::uint64_t> activeRenderTargetGeneration{0};

    // Guards the current-track format snapshot the control thread reads when it
    // decides, at arm time, whether the next track can be gaplessly spliced. The
    // render thread never takes this lock: it is written only by the control
    // thread (initial publish) and the event thread (after a splice) and read
    // only by the control thread (setNext gate).
    mutable std::mutex transitionMutex;
    std::optional<Format> optCurrentBackendFormat;
    std::optional<DecodedStreamInfo> optCurrentStreamInfo;

    // Marks the tiny window where the RT thread has consumed the lookahead
    // cursor and is about to publish the splice signal. Timeline ownership is
    // already stable, but control commands still wait so status/callback
    // publication is linearly settled before command bodies run.
    std::atomic<bool> spliceHandoffInProgress{false};

    mutable std::mutex stateMutex;
    std::uint64_t nextRenderTargetGeneration = 1;
    std::uint64_t nextSourceGeneration = 1;
    std::optional<PlaybackItem> optCurrentItem;
    Status status;
    std::function<void()> onTrackEnded;
    OnTrackAdvanced onTrackAdvanced;
    OnPlaybackFailure onPlaybackFailure;
    std::function<void()> onStateChanged;
    OnRouteChanged onRouteChanged;
    detail::RouteTracker routeTracker;
    DecoderFactoryFn decoderFactory;
    std::unique_ptr<EngineRenderTarget> renderTargetPtr;

    EngineEventQueue eventQueue;

    // Must be declared last so the PipeWire thread loop is stopped
    // before the callbacks and state it accesses are destroyed.
    std::unique_ptr<Backend> backendPtr;

    // ── Construction & Destruction ────────────────────────────────
    Impl(std::unique_ptr<Backend> backendPtr, Device device, DecoderFactoryFn decoderFactory)
      : currentDevice{std::move(device)}, decoderFactory{std::move(decoderFactory)}, backendPtr{std::move(backendPtr)}
    {
      syncBackendIdentity();
    }

    void startEventWorker(std::shared_ptr<Impl> const& selfPtr)
    {
      eventQueue.start(
        [selfPtr] -> std::optional<Notifications>
        {
          auto const lock = std::scoped_lock{selfPtr->controlMutex};

          if (selfPtr->lifecycleState != LifecycleState::Running)
          {
            return std::nullopt;
          }

          auto signal = RtSignal{};

          if (!selfPtr->eventQueue.tryPopRtSignal(signal))
          {
            return std::nullopt;
          }

          return selfPtr->processRtSignal(signal);
        },
        [selfPtr](PlaybackEvent& event)
        {
          auto const lock = std::scoped_lock{selfPtr->controlMutex};

          if (selfPtr->lifecycleState != LifecycleState::Running)
          {
            return Notifications{};
          }

          return selfPtr->processPlaybackEvent(event);
        });
    }

    ~Impl() { shutdown(); }

    void shutdown() noexcept
    {
      terminateOnException(
        [this]
        {
          {
            auto lock = std::unique_lock{controlMutex};

            if (lifecycleState == LifecycleState::Shutdown)
            {
              lock.unlock();
              eventQueue.waitForExit();
              return;
            }

            if (lifecycleState == LifecycleState::ShuttingDown)
            {
              // An external shutdown joins this worker. It must be allowed to
              // return from its callback before that join can complete.
              if (eventQueue.isCurrentThread())
              {
                return;
              }

              shutdownCv.wait(lock, [this] { return lifecycleState == LifecycleState::Shutdown; });
              lock.unlock();
              eventQueue.waitForExit();
              return;
            }

            waitForSpliceHandoff();
            settlePendingRtSignals();
            lifecycleState = LifecycleState::ShuttingDown;
            retireSessions();
          }

          eventQueue.stop([this](RtSignal const& signal) { discardSpliceSignalNode(signal.splicedNode); });

          {
            auto const lock = std::scoped_lock{controlMutex};

            if (backendPtr)
            {
              backendPtr->stop();
              backendPtr->close();
              resetRenderTarget();
            }

            timeline.clear();

            // The render thread is now stopped. Drain the signal ring once more to free
            // any splice signal it posted after the event queue stopped but before
            // the backend stopped, then free any armed-but-never-spliced lookahead node.
            eventQueue.drainRtSignals([this](RtSignal const& signal) { discardSpliceSignalNode(signal.splicedNode); });
            clearPreparedNext();
            backendPtr.reset();
            lifecycleState = LifecycleState::Shutdown;
          }

          shutdownCv.notify_all();
        });
    }

    Impl(Impl const&) = delete;
    Impl& operator=(Impl const&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    // ── Timeline publication ─────────────────────────────────────
    void publishCurrentNode(std::unique_ptr<TrackNode> nodePtr) { timeline.publishCurrent(std::move(nodePtr)); }
    std::shared_ptr<PcmSource> currentSource() const { return timeline.current(); }

    // ── Transition state ──────────────────────────────────────────
    static bool isGaplessCapable(DecodedStreamInfo const& info) noexcept
    {
      return !info.isLossy &&
             (info.codec == AudioCodec::Flac || info.codec == AudioCodec::Alac || info.codec == AudioCodec::Wav);
    }

    static bool canSplice(DecodedStreamInfo const& currentInfo,
                          Format const& currentBackendFormat,
                          TrackNode const& preparedNext) noexcept
    {
      return isGaplessCapable(currentInfo) && isGaplessCapable(preparedNext.info) &&
             currentBackendFormat == preparedNext.backendFormat;
    }

    void waitForSpliceHandoff() const noexcept
    {
      while (spliceHandoffInProgress.load(std::memory_order_acquire))
      {
        std::this_thread::yield();
      }
    }

    TrackNode* takeSplicedNode(TrackNode* node) const noexcept
    {
      return (node != nullptr && timeline.activeNode() == node) ? node : nullptr;
    }

    void discardSpliceSignalNode(TrackNode* node) noexcept { timeline.dropDisarmedLookahead(node); }

    // Arm `session` as the gapless successor. Control thread only. Any previous
    // successor is first disarmed or, if the render thread has already consumed
    // it, settled through the splice signal path before the new node is
    // published.
    void armPreparedNext(TrackNode&& session)
    {
      clearPreparedNext();
      timeline.armLookahead(std::make_unique<TrackNode>(std::move(session)));
    }

    void resetTransitionState()
    {
      {
        auto const lock = std::scoped_lock{transitionMutex};
        optCurrentBackendFormat.reset();
        optCurrentStreamInfo.reset();
      }
      clearPreparedNext();
    }

    // Drop the lookahead successor if it is still armed in the timeline. If the
    // render thread has already consumed the lookahead cursor and is between
    // active-node publish and signal enqueue, wait until the signal is visible
    // and settle it before the caller can act on currentSource()/status.
    // Requires controlMutex when render callbacks may still be active.
    std::optional<PlaybackItemId> clearPreparedNext()
    {
      auto* const disarmed = timeline.disarmLookahead();
      waitForSpliceHandoff();
      settlePendingRtSignals();

      auto optDisarmedItemId = std::optional<PlaybackItemId>{};

      if (disarmed != nullptr)
      {
        optDisarmedItemId = disarmed->item.id;
      }

      timeline.dropDisarmedLookahead(disarmed);
      return optDisarmedItemId;
    }

    bool clearPreparedNextForGeneration(std::uint64_t sourceGeneration)
    {
      waitForSpliceHandoff();
      settlePendingRtSignals();

      auto* const session = timeline.lookaheadNode();

      if (session == nullptr || session->sourceGeneration != sourceGeneration)
      {
        return false;
      }

      if (auto* const disarmed = timeline.disarmLookahead(); disarmed == session)
      {
        timeline.dropDisarmedLookahead(disarmed);
        return true;
      }

      return false;
    }

    void publishCurrentTransitionState(TrackNode const& session)
    {
      auto const lock = std::scoped_lock{transitionMutex};
      optCurrentBackendFormat = session.backendFormat;
      optCurrentStreamInfo = session.info;
    }

    // ── Helpers ────────────────────────────────────────────────────
    void syncBackendIdentity()
    {
      status.backendId = backendPtr->backendId();
      status.profileId = backendPtr->profileId();
      status.currentDeviceId = currentDevice.id;
    }

    void syncBackendStatus()
    {
      if (auto const vol = backendPtr->get(props::kVolume); vol)
      {
        status.volume = *vol;
      }

      if (auto const mute = backendPtr->get(props::kMuted); mute)
      {
        status.muted = *mute;
      }

      auto const volProp = backendPtr->queryProperty(PropertyId::Volume);
      status.volumeAvailable = volProp.isAvailable;
      status.volumeIsHardwareAssisted = volProp.isHardwareAssisted;
    }

    void cancelPendingDrainSignal() noexcept
    {
      playbackDrainPending.store(false, std::memory_order_release);
      drainEpoch.fetch_add(1, std::memory_order_acq_rel);
    }

    void resetEngine()
    {
      optCurrentItem.reset();
      timeline.retireCursor();
      backendStarted = false;
      cancelPendingDrainSignal();
      status = {};
      syncBackendIdentity();
      syncBackendStatus();
      accumulatedFrames.store(0, std::memory_order_relaxed);
      engineSampleRate.store(0, std::memory_order_relaxed);
      engineFrameBytes.store(0, std::memory_order_relaxed);
      routeTracker.clear();
    }

    // Non-RT event producers (backend / route / format / property / source
    // errors). Allowed to allocate and lock; must never be called from the RT
    // render thread.
    void enqueuePlaybackEvent(PlaybackEvent event) { eventQueue.enqueuePlaybackEvent(std::move(event)); }

    // Wait-free producer for the render thread. Pushes a trivially copyable
    // signal onto the lock-free ring and releases the semaphore; no lock, no
    // allocation, no unbounded work. Returns false only if the ring is full,
    // which cannot happen in practice (the event thread drains on every wake and
    // the render thread posts at most one signal per track).
    bool enqueueRtSignal(RtSignal signal) noexcept { return eventQueue.enqueueRtSignal(signal); }

    // Control-command entry point: acquires controlMutex and settles every
    // pending splice signal still in the ring before the command body runs.
    // Between the render thread's raw-pointer publish and the splice signal
    // application, currentSource(), the status fields, and the transition-format
    // snapshot still describe the retired track; settling first closes that
    // window, so a command like seek can never act on the wrong source. Pending
    // drain completions are forwarded to the normal event queue instead: a
    // control command may retire or reposition the render target, and the drain
    // must be checked after that command has applied.
    ControlLock lockControl() { return ControlLock{*this}; }

    // Requires controlMutex; every ring consumer holds it, which serializes the
    // pops the SPSC ring needs and makes splice settlement atomic for control
    // commands. The notifications a settled splice produces are forwarded to
    // the event worker instead of running here: user callbacks must keep
    // originating from the worker thread, and running them under controlMutex
    // would deadlock on a reentrant Engine call.
    void settlePendingRtSignals()
    {
      eventQueue.drainRtSignals(
        [this](RtSignal const& signal)
        {
          if (signal.kind == RtSignalKind::Drained)
          {
            assert(signal.splicedNode == nullptr);
            enqueuePlaybackEvent(DrainCompleteEvent{.generation = signal.generation, .drainEpoch = signal.drainEpoch});
            return;
          }

          if (auto notifications = processRtSignal(signal); !notifications.empty())
          {
            enqueuePlaybackEvent(DeferredNotifications{.notifications = std::move(notifications)});
          }
        });
    }

    // Dispatch a render-thread signal. Spliced signals carry a non-owning
    // TrackNode pointer; timeline ownership is already stable. Runs on the
    // event thread under controlMutex.
    Notifications processRtSignal(RtSignal const& signal)
    {
      switch (signal.kind)
      {
        case RtSignalKind::Spliced:
          return processSpliceAdvancedSignal(takeSplicedNode(signal.splicedNode), signal.generation);
        case RtSignalKind::Drained: return processDrainCompleteSignal(signal.generation, signal.drainEpoch);
      }

      return {};
    }

    struct EngineRenderTarget final : public RenderTarget
    {
      EngineRenderTarget(Impl& ownerRef, BackendId backendIdValue, std::uint64_t generationValue)
        : owner{ownerRef}, backendId{std::move(backendIdValue)}, generation{generationValue}
      {
      }

      RenderPcmResult renderPcm(std::span<std::byte> output) noexcept override
      {
        return detail::renderPcm(
          owner.timeline,
          owner.engineFrameBytes,
          owner.playbackDrainPending,
          generation,
          output,
          [this](std::uint64_t signalGeneration) noexcept { return owner.isActiveRenderTarget(signalGeneration); },
          [this](std::uint64_t signalGeneration) noexcept { return owner.trySplicePreparedNext(signalGeneration); });
      }

      void handleUnderrun() noexcept override
      {
        if (owner.isActiveRenderTarget(generation))
        {
          ++owner.underrunCount;
        }
      }

      void handlePositionAdvanced(std::uint32_t frames) noexcept override
      {
        if (owner.isActiveRenderTarget(generation))
        {
          owner.accumulatedFrames.fetch_add(frames, std::memory_order_relaxed);
        }
      }

      void handleDrainComplete() noexcept override
      {
        if (!owner.isActiveRenderTarget(generation))
        {
          return;
        }

        auto const signalDrainEpoch = owner.drainEpoch.load(std::memory_order_acquire);

        if (!owner.playbackDrainPending.exchange(false, std::memory_order_acq_rel))
        {
          return;
        }

        std::ignore = owner.enqueueRtSignal(
          RtSignal{.kind = RtSignalKind::Drained, .generation = generation, .drainEpoch = signalDrainEpoch});
      }

      void handleRouteReady(std::string_view routeAnchor) noexcept override
      {
        terminateOnException(
          [&]
          {
            if (!owner.isActiveRenderTarget(generation))
            {
              return;
            }

            owner.enqueuePlaybackEvent(RouteReadyEvent{
              .generation = generation, .backendId = backendId, .routeAnchor = std::string{routeAnchor}});
          });
      }

      void handleFormatChanged(Format const& format) noexcept override
      {
        terminateOnException(
          [&]
          {
            if (owner.isActiveRenderTarget(generation))
            {
              owner.enqueuePlaybackEvent(FormatChangedEvent{.generation = generation, .format = format});
            }
          });
      }

      void handlePropertyChanged(PropertySnapshot snapshot) noexcept override
      {
        terminateOnException(
          [&]
          {
            if (!owner.isActiveRenderTarget(generation))
            {
              return;
            }

            owner.enqueuePlaybackEvent(PropertyChangedEvent{.generation = generation, .snapshot = std::move(snapshot)});
          });
      }

      void handleBackendError(std::string_view message) noexcept override
      {
        terminateOnException(
          [&]
          {
            if (!owner.isActiveRenderTarget(generation))
            {
              return;
            }

            owner.enqueuePlaybackEvent(BackendErrorEvent{.generation = generation, .message = std::string{message}});
          });
      }

      Impl& owner;
      BackendId backendId;
      std::uint64_t generation = 0;
    };

    bool isActiveRenderTarget(std::uint64_t generation) const noexcept
    {
      return activeRenderTargetGeneration.load(std::memory_order_acquire) == generation;
    }

    // Wait-free gapless splice, executed on the RT render thread at a drain
    // boundary. No lock, no allocation, no unbounded work: consume the lookahead
    // cursor, publish its node as active, and post a non-owning signal. Engine's
    // timeline keeps both current and lookahead nodes alive.
    bool trySplicePreparedNext(std::uint64_t generation) noexcept
    {
      return detail::trySplicePreparedNext(
        timeline,
        spliceHandoffInProgress,
        generation,
        [this](std::uint64_t signalGeneration) noexcept { return isActiveRenderTarget(signalGeneration); },
        accumulatedFrames,
        engineSampleRate,
        engineFrameBytes,
        underrunCount,
        playbackDrainPending,
        [this](std::uint64_t signalGeneration, TrackNode* session) noexcept
        {
          std::ignore = enqueueRtSignal(
            RtSignal{.kind = RtSignalKind::Spliced, .generation = signalGeneration, .splicedNode = session});
        });
    }

    void retireRenderTarget() noexcept { activeRenderTargetGeneration.store(0, std::memory_order_release); }

    void retireSessions() noexcept
    {
      activeRenderTargetGeneration.store(0, std::memory_order_release);
      timeline.retireCursor();
    }

    void resetRenderTarget() noexcept { renderTargetPtr.reset(); }

    RenderTarget* createRenderTarget(Backend& backend)
    {
      auto const generation = nextRenderTargetGeneration++;
      renderTargetPtr = std::make_unique<EngineRenderTarget>(*this, backend.backendId(), generation);
      activeRenderTargetGeneration.store(generation, std::memory_order_release);
      return renderTargetPtr.get();
    }

    // ── Playback State Helpers ─────────────────────────────────────
    void resetPlaybackStatePreservingOutput()
    {
      auto const backendId = status.backendId;
      auto const profileId = status.profileId;
      auto const currentDeviceId = status.currentDeviceId;
      auto const volume = status.volume;
      auto const muted = status.muted;
      auto const volumeAvailable = status.volumeAvailable;
      auto const volumeIsHardwareAssisted = status.volumeIsHardwareAssisted;

      optCurrentItem.reset();
      timeline.retireCursor();
      backendStarted = false;
      cancelPendingDrainSignal();
      status = {};
      status.backendId = backendId;
      status.profileId = profileId;
      status.currentDeviceId = currentDeviceId;
      status.volume = volume;
      status.muted = muted;
      status.volumeAvailable = volumeAvailable;
      status.volumeIsHardwareAssisted = volumeIsHardwareAssisted;
      accumulatedFrames.store(0, std::memory_order_relaxed);
      engineSampleRate.store(0, std::memory_order_relaxed);
      engineFrameBytes.store(0, std::memory_order_relaxed);
      routeTracker.clear();
    }

    void closeBackendPlayback()
    {
      backendPtr->stop();
      backendPtr->close();
      resetRenderTarget();
      timeline.clear();
    }

    PendingRouteNotifications capturePendingRouteNotifications()
    {
      return PendingRouteNotifications{
        .stateChanged = onStateChanged,
        .routeChanged = onRouteChanged,
        .routeSnapshot = RouteStatus{.state = routeTracker.state(), .optAnchor = routeTracker.anchor()}};
    }

    // ── Playback State Transitions ──────────────────────────────────
    Notifications completeDrain(std::uint64_t generation, std::uint64_t signalDrainEpoch)
    {
      auto onTrackEndedCallback = std::function<void()>{};
      auto onRouteChangedCallback = OnRouteChanged{};
      auto stateChanged = std::function<void()>{};

      {
        auto const lock = std::scoped_lock{stateMutex};

        if (!isActiveRenderTarget(generation) || drainEpoch.load(std::memory_order_acquire) != signalDrainEpoch)
        {
          return {};
        }

        retireRenderTarget();
        onRouteChangedCallback = onRouteChanged;
        resetPlaybackStatePreservingOutput();
        onTrackEndedCallback = onTrackEnded;
        stateChanged = onStateChanged;
      }

      resetTransitionState();
      closeBackendPlayback();

      auto notifications = Notifications{};
      appendStateChangedNotification(notifications, std::move(stateChanged));

      if (onRouteChangedCallback)
      {
        notifications.emplace_back([callback = std::move(onRouteChangedCallback)] { callback({}); });
      }

      if (onTrackEndedCallback)
      {
        notifications.emplace_back([callback = std::move(onTrackEndedCallback)] { callback(); });
      }

      return notifications;
    }

    // ── Playback Event Dispatch ────────────────────────────────────
    // Non-const: DeferredNotifications hands its payload out by move.
    Notifications processPlaybackEvent(PlaybackEvent& event)
    {
      return std::visit([this](auto& typedEvent) { return processPlaybackEvent(typedEvent); }, event);
    }

    Notifications processPlaybackEvent(BackendErrorEvent const& event) { return processBackendErrorEvent(event); }

    Notifications processPlaybackEvent(SourceErrorEvent const& event) { return processSourceErrorEvent(event); }

    Notifications processPlaybackEvent(DrainCompleteEvent const& event) { return processDrainCompleteEvent(event); }

    Notifications processPlaybackEvent(RouteReadyEvent const& event) { return processRouteReadyEvent(event); }

    Notifications processPlaybackEvent(FormatChangedEvent const& event) { return processFormatChangedEvent(event); }

    Notifications processPlaybackEvent(PropertyChangedEvent const& event) { return processPropertyChangedEvent(event); }

    Notifications processPlaybackEvent(DeferredNotifications& event) { return std::move(event.notifications); }

    // ── Notification Builders ──────────────────────────────────────
    static void appendStateChangedNotification(Notifications& notifications, std::function<void()> callback)
    {
      if (callback)
      {
        notifications.emplace_back([callback = std::move(callback)] { callback(); });
      }
    }

    static void appendRouteNotification(Notifications& notifications, OnRouteChanged callback, RouteStatus snapshot)
    {
      if (callback)
      {
        notifications.emplace_back([callback = std::move(callback), snapshot = std::move(snapshot)]
                                   { callback(snapshot); });
      }
    }

    static void appendPendingRouteNotifications(Notifications& notifications, PendingRouteNotifications pending)
    {
      appendStateChangedNotification(notifications, std::move(pending.stateChanged));
      appendRouteNotification(notifications, std::move(pending.routeChanged), std::move(pending.routeSnapshot));
    }

    static void appendTrackAdvancedNotification(Notifications& notifications,
                                                OnTrackAdvanced callback,
                                                TrackAdvanced event)
    {
      if (callback)
      {
        notifications.emplace_back([callback = std::move(callback), event = std::move(event)] { callback(event); });
      }
    }

    static void appendPlaybackFailureNotification(Notifications& notifications,
                                                  OnPlaybackFailure callback,
                                                  PlaybackFailure failure)
    {
      if (callback)
      {
        notifications.emplace_back([callback = std::move(callback), failure = std::move(failure)]
                                   { callback(failure); });
      }
    }

    // ── Render Signal Reducers ─────────────────────────────────────
    // Complete a gapless splice on the event thread. The render thread already
    // made `session`'s source active; here we install the owning shared_ptr,
    // retire the previous source (destroying it — and joining its decode thread —
    // off the RT thread), refresh the current-track format the next arm gates
    // against, and surface the advance to observers. If the render target was
    // retired between the splice and now, the session is simply dropped (its
    // source is destroyed here, still off the RT thread).
    Notifications processSpliceAdvancedSignal(TrackNode* session, std::uint64_t generation)
    {
      if (session == nullptr)
      {
        return {};
      }

      auto notifications = Notifications{};
      auto trackAdvanced = OnTrackAdvanced{};
      auto stateChanged = std::function<void()>{};
      auto routeChanged = OnRouteChanged{};
      auto routeSnapshot = RouteStatus{};

      {
        auto const lock = std::scoped_lock{stateMutex};

        if (!isActiveRenderTarget(generation))
        {
          return {};
        }

        auto const& info = session->info;
        optCurrentItem = session->item;
        status.duration = info.duration;
        status.elapsed = std::chrono::milliseconds{0};
        status.statusText.clear();
        status.transport = Transport::Playing;
        routeTracker.setDecoder(info.sourceFormat, info.outputFormat, info.isLossy, info.codec);
        routeTracker.setEngineFormat(info.outputFormat);
        status.routeState = routeTracker.state();

        trackAdvanced = onTrackAdvanced;
        stateChanged = onStateChanged;
        routeChanged = onRouteChanged;
        routeSnapshot = RouteStatus{.state = routeTracker.state(), .optAnchor = routeTracker.anchor()};
      }

      // Refresh the current-track format so the next armed successor is gated
      // against the track that is now playing.
      {
        auto const lock = std::scoped_lock{transitionMutex};
        optCurrentBackendFormat = session->backendFormat;
        optCurrentStreamInfo = session->info;
      }

      // Promote the lookahead node to current; the retired node is destroyed
      // when `retired` leaves scope, after all locks are released.
      auto const retiredPtr = timeline.promoteSplicedLookahead(session);

      appendTrackAdvancedNotification(notifications,
                                      std::move(trackAdvanced),
                                      TrackAdvanced{.itemId = session->item.id, .input = session->item.input});
      appendStateChangedNotification(notifications, std::move(stateChanged));
      appendRouteNotification(notifications, std::move(routeChanged), std::move(routeSnapshot));
      return notifications;
    }

    Notifications processDrainCompleteSignal(std::uint64_t generation, std::uint64_t signalDrainEpoch)
    {
      return completeDrain(generation, signalDrainEpoch);
    }

    // ── Playback Event Reducers ────────────────────────────────────
    Notifications processBackendErrorEvent(BackendErrorEvent const& event)
    {
      auto stateChanged = std::function<void()>{};
      auto failureCallback = OnPlaybackFailure{};
      auto optFailedItem = std::optional<PlaybackItem>{};
      {
        auto const lock = std::scoped_lock{stateMutex};

        if (!isActiveRenderTarget(event.generation))
        {
          return {};
        }

        optFailedItem = optCurrentItem;
        retireRenderTarget();
        resetPlaybackStatePreservingOutput();
        status.transport = Transport::Error;
        status.statusText = event.message;
        failureCallback = onPlaybackFailure;
        stateChanged = onStateChanged;
      }

      resetTransitionState();
      closeBackendPlayback();

      auto notifications = Notifications{};

      if (optFailedItem)
      {
        appendPlaybackFailureNotification(notifications,
                                          std::move(failureCallback),
                                          PlaybackFailure{
                                            .kind = PlaybackFailureKind::DeviceLost,
                                            .itemId = optFailedItem->id,
                                            .input = optFailedItem->input,
                                            .generation = event.generation,
                                            .error = Error{.code = Error::Code::IoError, .message = event.message},
                                            .recoverable = false,
                                          });
      }

      appendStateChangedNotification(notifications, std::move(stateChanged));
      return notifications;
    }

    Notifications processSourceErrorEvent(SourceErrorEvent const& event)
    {
      if (clearPreparedNextForGeneration(event.sourceGeneration))
      {
        return {};
      }

      auto const message = event.error.message.empty() ? std::string{"PCM source failed"} : event.error.message;
      auto endedCallback = std::function<void()>{};
      auto failureCallback = OnPlaybackFailure{};
      auto optFailedItem = std::optional<PlaybackItem>{};
      auto stateChanged = std::function<void()>{};

      {
        auto const lock = std::scoped_lock{stateMutex};

        if (event.sourceGeneration != timeline.activeSourceGeneration())
        {
          return {};
        }

        if (status.transport == Transport::Idle)
        {
          return {};
        }

        optFailedItem = optCurrentItem;
        retireRenderTarget();
        resetPlaybackStatePreservingOutput();
        status.transport = Transport::Error;
        status.statusText = message;
        endedCallback = onTrackEnded;
        failureCallback = onPlaybackFailure;
        stateChanged = onStateChanged;
      }

      resetTransitionState();
      closeBackendPlayback();

      auto notifications = Notifications{};

      if (optFailedItem)
      {
        auto failureError = event.error;

        if (failureError.message.empty())
        {
          failureError.message = message;
        }

        appendPlaybackFailureNotification(notifications,
                                          std::move(failureCallback),
                                          PlaybackFailure{
                                            .kind = PlaybackFailureKind::Decode,
                                            .itemId = optFailedItem->id,
                                            .input = optFailedItem->input,
                                            .generation = event.sourceGeneration,
                                            .error = std::move(failureError),
                                            .recoverable = true,
                                          });
      }

      appendStateChangedNotification(notifications, std::move(stateChanged));

      if (endedCallback)
      {
        notifications.emplace_back([callback = std::move(endedCallback)] { callback(); });
      }

      return notifications;
    }

    Notifications processDrainCompleteEvent(DrainCompleteEvent const& event)
    {
      return completeDrain(event.generation, event.drainEpoch);
    }

    Notifications processRouteReadyEvent(RouteReadyEvent const& event)
    {
      auto notifications = Notifications{};
      auto pendingNotifications = PendingRouteNotifications{};
      {
        auto const lock = std::scoped_lock{stateMutex};

        if (!isActiveRenderTarget(event.generation))
        {
          return {};
        }

        routeTracker.setAnchor(event.backendId, event.routeAnchor);
        pendingNotifications = capturePendingRouteNotifications();
      }

      appendPendingRouteNotifications(notifications, std::move(pendingNotifications));
      return notifications;
    }

    Notifications processFormatChangedEvent(FormatChangedEvent const& event)
    {
      auto notifications = Notifications{};
      auto pendingNotifications = PendingRouteNotifications{};
      {
        auto const lock = std::scoped_lock{stateMutex};

        if (!isActiveRenderTarget(event.generation))
        {
          return {};
        }

        routeTracker.setEngineFormat(event.format);
        status.routeState.engineOutputFormat = event.format;
        pendingNotifications = capturePendingRouteNotifications();
      }

      appendPendingRouteNotifications(notifications, std::move(pendingNotifications));
      return notifications;
    }

    Notifications processPropertyChangedEvent(PropertyChangedEvent const& event)
    {
      auto notifications = Notifications{};
      auto pendingNotifications = PendingRouteNotifications{};
      {
        auto const lock = std::scoped_lock{stateMutex};

        if (!isActiveRenderTarget(event.generation))
        {
          return {};
        }

        if (event.snapshot.id == PropertyId::Volume)
        {
          if (event.snapshot.optValue)
          {
            if (auto const* vol = std::get_if<float>(&*event.snapshot.optValue); vol != nullptr)
            {
              status.volume = *vol;
            }
          }

          status.volumeAvailable = event.snapshot.info.isAvailable;
          status.volumeIsHardwareAssisted = event.snapshot.info.isHardwareAssisted;
        }
        else if (event.snapshot.id == PropertyId::Muted)
        {
          if (event.snapshot.optValue)
          {
            if (auto const* mute = std::get_if<bool>(&*event.snapshot.optValue); mute != nullptr)
            {
              status.muted = *mute;
            }
          }
        }
        else
        {
          return {};
        }

        pendingNotifications = capturePendingRouteNotifications();
      }

      appendPendingRouteNotifications(notifications, std::move(pendingNotifications));
      return notifications;
    }

    // ── Track opening ──────────────────────────────────────────────
    Result<TrackNode> openTrackSession(PlaybackItem const& item, std::uint64_t sourceGeneration)
    {
      auto session = detail::TrackSession::create(
        item.input,
        currentDevice,
        backendPtr->backendId(),
        backendPtr->profileId(),
        decoderFactory,
        [this, sourceGeneration](Error const& err)
        { enqueuePlaybackEvent(SourceErrorEvent{.sourceGeneration = sourceGeneration, .error = err}); });

      if (!session)
      {
        return std::unexpected{session.error()};
      }

      return TrackNode{.item = item,
                       .sourcePtr = std::move(session->sourcePtr),
                       .backendFormat = session->backendFormat,
                       .info = session->info,
                       .sourceGeneration = sourceGeneration};
    }

    void publishCurrentTrackState(TrackNode const& session)
    {
      // TrackSession::create() ran lock-free above; only the status/routeTracker
      // publication needs the lock, which status() also takes when it reads them
      // concurrently from the UI thread.
      auto const lock = std::scoped_lock{stateMutex};
      optCurrentItem = session.item;
      status.duration = session.info.duration;
      status.elapsed = std::chrono::milliseconds{0};
      accumulatedFrames.store(0, std::memory_order_relaxed);
      routeTracker.setDecoder(
        session.info.sourceFormat, session.info.outputFormat, session.info.isLossy, session.info.codec);
      routeTracker.setEngineFormat(session.info.outputFormat);
      status.routeState = routeTracker.state();
      engineSampleRate.store(session.info.outputFormat.sampleRate, std::memory_order_relaxed);
      engineFrameBytes.store(
        static_cast<std::uint32_t>(frameBytes(session.info.outputFormat)), std::memory_order_relaxed);
    }

    void setBackendUnlocked(std::unique_ptr<Backend> nextBackendPtr, Device const& device);
    void updateDeviceUnlocked(Device const& device);
    Result<> applyInitialOffset(TrackNode& node, std::chrono::milliseconds initialOffset);
    void playUnlocked(PlaybackItem const& item, std::chrono::milliseconds initialOffset = {});
    Result<PreparedNextResult> setNextUnlocked(PlaybackItem const& item);
    std::optional<PlaybackItemId> clearNextUnlocked();
    void pauseUnlocked();
    void resumeUnlocked();
    void stopUnlocked();
    void seekUnlocked(std::chrono::milliseconds offset);
    Result<> setVolumeUnlocked(float volume);
    Result<> setMutedUnlocked(bool muted);

    // Marks the route-activation attempt failed with a terminal Error state
    // and a non-recoverable RouteActivation failure notification. Caller must
    // hold stateMutex and own any side cleanup (render target teardown,
    // transition reset) before invoking.
    void markRouteActivationFailureUnlocked(Notifications& notifications,
                                            PlaybackItem const& item,
                                            std::uint64_t sourceGeneration,
                                            Error error);
  };

  Engine::Impl::ControlLock::~ControlLock() = default;

  void Engine::Impl::setBackendUnlocked(std::unique_ptr<Backend> nextBackendPtr, Device const& device)
  {
    struct State
    {
      std::optional<PlaybackItem> optItem;
      std::chrono::milliseconds elapsed{0};
      bool wasPlaying = false;
    };

    auto const state = [this]
    {
      auto const lock = std::scoped_lock{stateMutex};
      return State{
        .optItem = optCurrentItem,
        .elapsed =
          [this]
        {
          auto const frames = accumulatedFrames.load(std::memory_order_relaxed);
          auto const sr = engineSampleRate.load(std::memory_order_relaxed);
          return samplesToDuration(frames, sr);
        }(),
        .wasPlaying = (status.transport == Transport::Playing),
      };
    }();

    stopUnlocked();
    backendPtr = std::move(nextBackendPtr);
    currentDevice = device;
    {
      auto const lock = std::scoped_lock{stateMutex};
      status = {};
      syncBackendIdentity();
      syncBackendStatus();
    }

    if (state.optItem)
    {
      playUnlocked(*state.optItem, state.elapsed);

      if (!state.wasPlaying)
      {
        pauseUnlocked();
      }
    }
  }

  void Engine::Impl::updateDeviceUnlocked(Device const& device)
  {
    currentDevice = device;
    clearPreparedNext();
  }

  Result<> Engine::Impl::applyInitialOffset(TrackNode& node, std::chrono::milliseconds const initialOffset)
  {
    if (initialOffset <= std::chrono::milliseconds{0})
    {
      return {};
    }

    if (!node.sourcePtr)
    {
      return makeError(Error::Code::InvalidState, "Cannot seek restored playback without an active source");
    }

    if (auto const seekResult = node.sourcePtr->seek(initialOffset); !seekResult)
    {
      return std::unexpected{seekResult.error()};
    }

    auto const sr = engineSampleRate.load(std::memory_order_relaxed);
    accumulatedFrames.store(durationToSamples(initialOffset, sr), std::memory_order_relaxed);
    return {};
  }

  void Engine::Impl::markRouteActivationFailureUnlocked(Notifications& notifications,
                                                        PlaybackItem const& item,
                                                        std::uint64_t const sourceGeneration,
                                                        Error error)
  {
    status.transport = Transport::Error;
    status.statusText = error.message;
    timeline.retireCursor();
    optCurrentItem.reset();
    appendPlaybackFailureNotification(notifications,
                                      onPlaybackFailure,
                                      PlaybackFailure{
                                        .kind = PlaybackFailureKind::RouteActivation,
                                        .itemId = item.id,
                                        .input = item.input,
                                        .generation = sourceGeneration,
                                        .error = std::move(error),
                                        .recoverable = false,
                                      });
  }

  void Engine::Impl::playUnlocked(PlaybackItem const& item, std::chrono::milliseconds const initialOffset)
  {
    resetTransitionState();

    {
      auto const lock = std::scoped_lock{stateMutex};
      retireRenderTarget();
      timeline.retireCursor();
    }

    backendPtr->stop();
    backendPtr->close();
    resetRenderTarget();
    timeline.clear();

    auto const sourceGeneration = nextSourceGeneration++;

    {
      auto const lock = std::scoped_lock{stateMutex};
      underrunCount = 0;
      routeTracker.clear();
      backendStarted = false;
      cancelPendingDrainSignal();
      status.transport = Transport::Opening;
      optCurrentItem = item;
      syncBackendIdentity();
    }

    auto openedTrack = openTrackSession(item, sourceGeneration);

    if (!openedTrack)
    {
      auto notifications = Notifications{};
      {
        auto const lock = std::scoped_lock{stateMutex};
        status.transport = Transport::Error;
        status.statusText = openedTrack.error().message;
        timeline.retireCursor();
        optCurrentItem.reset();
        appendPlaybackFailureNotification(notifications,
                                          onPlaybackFailure,
                                          PlaybackFailure{
                                            .kind = PlaybackFailureKind::TrackOpen,
                                            .itemId = item.id,
                                            .input = item.input,
                                            .generation = sourceGeneration,
                                            .error = openedTrack.error(),
                                            .recoverable = true,
                                          });
      }

      if (!notifications.empty())
      {
        enqueuePlaybackEvent(DeferredNotifications{.notifications = std::move(notifications)});
      }

      return;
    }

    publishCurrentTransitionState(*openedTrack);
    publishCurrentTrackState(*openedTrack);

    {
      auto const lock = std::scoped_lock{stateMutex};
      status.transport = Transport::Buffering;
    }

    auto openedNodePtr = std::make_unique<TrackNode>(std::move(*openedTrack));
    auto* const currentNode = openedNodePtr.get();

    if (auto const seekResult = applyInitialOffset(*currentNode, initialOffset); !seekResult)
    {
      auto notifications = Notifications{};
      {
        auto const lock = std::scoped_lock{stateMutex};
        markRouteActivationFailureUnlocked(notifications, item, sourceGeneration, seekResult.error());
      }
      timeline.clear();
      resetTransitionState();

      if (!notifications.empty())
      {
        enqueuePlaybackEvent(DeferredNotifications{.notifications = std::move(notifications)});
      }

      return;
    }

    publishCurrentNode(std::move(openedNodePtr));
    auto* renderTarget = createRenderTarget(*backendPtr);

    if (auto const openResult = backendPtr->open(currentNode->backendFormat, renderTarget); !openResult)
    {
      retireRenderTarget();
      backendPtr->close();
      resetRenderTarget();
      auto notifications = Notifications{};
      {
        auto const lock = std::scoped_lock{stateMutex};
        markRouteActivationFailureUnlocked(notifications, item, sourceGeneration, openResult.error());
      }
      timeline.clear();
      resetTransitionState();

      if (!notifications.empty())
      {
        enqueuePlaybackEvent(DeferredNotifications{.notifications = std::move(notifications)});
      }

      return;
    }

    {
      auto const lock = std::scoped_lock{stateMutex};
      syncBackendStatus();
    }

    auto const bufferedDuration =
      currentNode->sourcePtr ? currentNode->sourcePtr->bufferedDuration() : std::chrono::milliseconds{0};

    if (auto const drained = !currentNode->sourcePtr || currentNode->sourcePtr->isDrained();
        drained && bufferedDuration == std::chrono::milliseconds{0})
    {
      retireRenderTarget();
      backendPtr->stop();
      backendPtr->close();
      resetRenderTarget();
      timeline.clear();
      resetTransitionState();
      auto const lock = std::scoped_lock{stateMutex};
      resetEngine();
      return;
    }

    {
      auto const lock = std::scoped_lock{stateMutex};

      // Error is terminal: if an already-applied error moved the transport to
      // Error, never clobber it with Playing. Source/backend errors that arrive
      // while this control command is running are queued behind controlMutex and
      // will be applied after this command returns.
      if (status.transport == Transport::Error)
      {
        return;
      }

      status.transport = Transport::Playing;
      backendStarted = true;
    }

    backendPtr->start();
  }

  Result<Engine::PreparedNextResult> Engine::Impl::setNextUnlocked(PlaybackItem const& item)
  {
    bool haveActive = false;
    {
      auto const lock = std::scoped_lock{transitionMutex};
      haveActive = optCurrentBackendFormat && optCurrentStreamInfo;
    }

    if (!haveActive)
    {
      clearPreparedNext();
      return makeError(Error::Code::InvalidState, "No active playback to prepare next track");
    }

    auto const sourceGeneration = nextSourceGeneration++;
    auto openedTrack = openTrackSession(item, sourceGeneration);

    if (!openedTrack)
    {
      clearPreparedNext();
      return std::unexpected{openedTrack.error()};
    }

    // Decide gapless capability now, on the control thread, against the track
    // that is currently playing. The current format only changes on a splice
    // (which consumes the lookahead cursor) or a control command (which clears
    // it), so this verdict stays valid until the render thread consumes the
    // successor.
    bool stillActive = false;
    bool capable = false;
    {
      auto const lock = std::scoped_lock{transitionMutex};
      stillActive = optCurrentBackendFormat && optCurrentStreamInfo;

      if (stillActive)
      {
        capable = canSplice(*optCurrentStreamInfo, *optCurrentBackendFormat, *openedTrack);
      }
    }

    if (!stillActive)
    {
      clearPreparedNext();
      return makeError(Error::Code::InvalidState, "Active playback changed before the next track was prepared");
    }

    if (capable)
    {
      armPreparedNext(std::move(*openedTrack));
    }
    else
    {
      // Not gapless-capable (lossy source, or a format the current route cannot
      // hold): leave the lookahead cursor empty so the render thread drains and the
      // caller performs an ordinary (re-opening) transition. The item id is still
      // returned so the caller knows the successor is playable.
      clearPreparedNext();
    }

    return PreparedNextResult{
      .itemId = item.id,
      .transition = capable ? PreparedTransitionMode::Gapless : PreparedTransitionMode::DrainFallback,
    };
  }

  std::optional<Engine::PlaybackItemId> Engine::Impl::clearNextUnlocked()
  {
    return clearPreparedNext();
  }

  void Engine::Impl::pauseUnlocked()
  {
    bool shouldPause = false;
    {
      auto const lock = std::scoped_lock{stateMutex};

      if (status.transport == Transport::Playing || status.transport == Transport::Buffering)
      {
        status.transport = Transport::Paused;
        shouldPause = backendStarted.load();
      }
    }

    if (shouldPause)
    {
      backendPtr->pause();
    }
  }

  void Engine::Impl::resumeUnlocked()
  {
    auto const sourcePtr = currentSource();
    auto lock = std::unique_lock{stateMutex};

    if (status.transport != Transport::Paused)
    {
      return;
    }

    if (backendStarted)
    {
      status.transport = Transport::Playing;
      lock.unlock();
      backendPtr->resume();
      return;
    }

    if (auto const drained = !sourcePtr || sourcePtr->isDrained();
        drained &&
        (sourcePtr ? sourcePtr->bufferedDuration() : std::chrono::milliseconds{0}) == std::chrono::milliseconds{0})
    {
      retireRenderTarget();
      resetEngine();
      lock.unlock();
      timeline.clear();
      return;
    }

    status.transport = Transport::Playing;
    backendStarted = true;
    lock.unlock();
    backendPtr->start();
  }

  void Engine::Impl::stopUnlocked()
  {
    resetTransitionState();

    {
      auto const lock = std::scoped_lock{stateMutex};
      retireRenderTarget();
      timeline.retireCursor();
    }

    backendPtr->stop();
    backendPtr->close();
    resetRenderTarget();

    {
      auto const lock = std::scoped_lock{stateMutex};
      resetEngine();
    }

    timeline.clear();
  }

  void Engine::Impl::seekUnlocked(std::chrono::milliseconds offset)
  {
    clearPreparedNext();

    auto const sourcePtr = currentSource();

    if (!sourcePtr)
    {
      return;
    }

    cancelPendingDrainSignal();

    bool wasPaused = false;
    {
      auto const lock = std::scoped_lock{stateMutex};
      wasPaused = (status.transport == Transport::Paused);
      status.transport = Transport::Buffering;
      status.elapsed = offset;
      auto const sr = engineSampleRate.load(std::memory_order_relaxed);
      accumulatedFrames.store(durationToSamples(offset, sr), std::memory_order_relaxed);
      status.statusText.clear();
    }

    backendPtr->stop();
    backendPtr->flush();
    backendStarted = false;

    if (auto const seekResult = sourcePtr->seek(offset); !seekResult)
    {
      auto const lock = std::scoped_lock{stateMutex};
      status.transport = Transport::Error;
      status.statusText = seekResult.error().message;
      return;
    }

    auto const bufferedDuration = sourcePtr->bufferedDuration();

    if (auto const drained = sourcePtr->isDrained(); drained && bufferedDuration == std::chrono::milliseconds{0})
    {
      // Retire before stopping: a drain callback still in flight from this
      // (about to be closed) target must not be applied later and resurrect
      // or re-quiesce engine state the reset below already settled.
      retireRenderTarget();
      backendPtr->stop();
      backendPtr->close();
      resetRenderTarget();
      timeline.clear();
      resetTransitionState();
      auto const lock = std::scoped_lock{stateMutex};
      resetEngine();
      return;
    }

    if (wasPaused)
    {
      auto const lock = std::scoped_lock{stateMutex};
      status.transport = Transport::Paused;
      return;
    }

    {
      auto const lock = std::scoped_lock{stateMutex};

      // Error is terminal: if an already-applied error moved the transport to
      // Error, never clobber it with Playing. Source/backend errors that arrive
      // while this control command is running are queued behind controlMutex and
      // will be applied after this command returns.
      if (status.transport == Transport::Error)
      {
        return;
      }

      status.transport = Transport::Playing;
      backendStarted = true;
    }

    backendPtr->start();
  }

  Result<> Engine::Impl::setVolumeUnlocked(float volume)
  {
    // The requested value is the engine's intent regardless of whether the
    // backend accepted it, so cache it either way and hand the backend failure
    // back to the caller to report.
    auto result = backendPtr->set(props::kVolume, volume);

    auto const lock = std::scoped_lock{stateMutex};
    status.volume = volume;
    return result;
  }

  Result<> Engine::Impl::setMutedUnlocked(bool muted)
  {
    auto result = backendPtr->set(props::kMuted, muted);

    auto const lock = std::scoped_lock{stateMutex};
    status.muted = muted;
    return result;
  }

  // ── Engine ──────────────────────────────────────────────────────

  Engine::Engine(std::unique_ptr<Backend> backendPtr, Device const& device, DecoderFactoryFn decoderFactory)
    : _implPtr{std::make_shared<Impl>(std::move(backendPtr), device, std::move(decoderFactory))}
  {
    _implPtr->syncBackendStatus();
    _implPtr->startEventWorker(_implPtr);
  }

  Engine::~Engine()
  {
    shutdown();
  }

  void Engine::shutdown() noexcept
  {
    if (_implPtr)
    {
      _implPtr->shutdown();
    }
  }

  void Engine::setBackend(std::unique_ptr<Backend> backendPtr, Device const& device)
  {
    auto const controlLock = _implPtr->lockControl();

    if (!controlLock)
    {
      return;
    }

    _implPtr->setBackendUnlocked(std::move(backendPtr), device);
  }

  void Engine::updateDevice(Device const& device)
  {
    auto const controlLock = _implPtr->lockControl();

    if (!controlLock)
    {
      return;
    }

    _implPtr->updateDeviceUnlocked(device);
  }

  void Engine::setOnTrackEnded(std::function<void()> callback)
  {
    auto const lock = std::scoped_lock{_implPtr->stateMutex};
    _implPtr->onTrackEnded = std::move(callback);
  }

  void Engine::setOnTrackAdvanced(OnTrackAdvanced callback)
  {
    auto const lock = std::scoped_lock{_implPtr->stateMutex};
    _implPtr->onTrackAdvanced = std::move(callback);
  }

  void Engine::setOnPlaybackFailure(OnPlaybackFailure callback)
  {
    auto const lock = std::scoped_lock{_implPtr->stateMutex};
    _implPtr->onPlaybackFailure = std::move(callback);
  }

  void Engine::setOnRouteChanged(OnRouteChanged callback)
  {
    auto const lock = std::scoped_lock{_implPtr->stateMutex};
    _implPtr->onRouteChanged = std::move(callback);
  }

  void Engine::setOnStateChanged(std::function<void()> callback)
  {
    auto const lock = std::scoped_lock{_implPtr->stateMutex};
    _implPtr->onStateChanged = std::move(callback);
  }

  void Engine::defer(std::function<void()> callback)
  {
    if (!callback)
    {
      return;
    }

    auto notifications = Impl::Notifications{};
    notifications.push_back(std::move(callback));
    _implPtr->enqueuePlaybackEvent(Impl::DeferredNotifications{.notifications = std::move(notifications)});
  }

  Engine::RouteStatus Engine::routeStatus() const
  {
    auto const lock = std::scoped_lock{_implPtr->stateMutex};
    return RouteStatus{.state = _implPtr->routeTracker.state(), .optAnchor = _implPtr->routeTracker.anchor()};
  }

  Transport Engine::transport() const
  {
    auto const lock = std::scoped_lock{_implPtr->stateMutex};
    return _implPtr->status.transport;
  }

  BackendId Engine::backendId() const
  {
    auto const lock = std::scoped_lock{_implPtr->stateMutex};
    return _implPtr->status.backendId;
  }

  Engine::Status Engine::status() const
  {
    auto const sourcePtr = _implPtr->currentSource();
    auto const lock = std::scoped_lock{_implPtr->stateMutex};
    auto snap = Status{_implPtr->status};
    auto const totalFrames = _implPtr->accumulatedFrames.load(std::memory_order_relaxed);
    auto const sampleRate = _implPtr->engineSampleRate.load(std::memory_order_relaxed);
    snap.elapsed = samplesToDuration(totalFrames, sampleRate);
    snap.routeState = _implPtr->routeTracker.state();
    snap.bufferedDuration = sourcePtr ? sourcePtr->bufferedDuration() : std::chrono::milliseconds{0};
    snap.underrunCount = _implPtr->underrunCount.load(std::memory_order_relaxed);
    return snap;
  }

  void Engine::play(PlaybackItem const& item, std::chrono::milliseconds const initialOffset)
  {
    auto const controlLock = _implPtr->lockControl();

    if (!controlLock)
    {
      return;
    }

    _implPtr->playUnlocked(item, initialOffset);
  }

  Result<Engine::PreparedNextResult> Engine::setNext(PlaybackItem const& item)
  {
    auto const controlLock = _implPtr->lockControl();

    if (!controlLock)
    {
      return makeError(Error::Code::InvalidState, "Engine is shut down");
    }

    return _implPtr->setNextUnlocked(item);
  }

  std::optional<Engine::PlaybackItemId> Engine::clearNext()
  {
    auto const controlLock = _implPtr->lockControl();

    if (!controlLock)
    {
      return std::nullopt;
    }

    return _implPtr->clearNextUnlocked();
  }

  void Engine::pause()
  {
    auto const controlLock = _implPtr->lockControl();

    if (!controlLock)
    {
      return;
    }

    _implPtr->pauseUnlocked();
  }

  void Engine::resume()
  {
    auto const controlLock = _implPtr->lockControl();

    if (!controlLock)
    {
      return;
    }

    _implPtr->resumeUnlocked();
  }

  void Engine::stop()
  {
    auto const controlLock = _implPtr->lockControl();

    if (!controlLock)
    {
      return;
    }

    _implPtr->stopUnlocked();
  }

  void Engine::seek(std::chrono::milliseconds offset)
  {
    auto const controlLock = _implPtr->lockControl();

    if (!controlLock)
    {
      return;
    }

    _implPtr->seekUnlocked(offset);
  }

  Result<> Engine::setVolume(float volume)
  {
    auto const controlLock = _implPtr->lockControl();

    if (!controlLock)
    {
      return makeError(Error::Code::InvalidState, "Engine is shut down");
    }

    return _implPtr->setVolumeUnlocked(volume);
  }

  float Engine::volume() const
  {
    auto const lock = std::scoped_lock{_implPtr->stateMutex};
    return _implPtr->status.volume;
  }

  Result<> Engine::setMuted(bool muted)
  {
    auto const controlLock = _implPtr->lockControl();

    if (!controlLock)
    {
      return makeError(Error::Code::InvalidState, "Engine is shut down");
    }

    return _implPtr->setMutedUnlocked(muted);
  }

  bool Engine::isMuted() const
  {
    auto const lock = std::scoped_lock{_implPtr->stateMutex};
    return _implPtr->status.muted;
  }

  bool Engine::isVolumeAvailable() const
  {
    auto const lock = std::scoped_lock{_implPtr->stateMutex};
    return _implPtr->status.volumeAvailable;
  }
} // namespace ao::audio
