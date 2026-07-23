// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "detail/RenderPath.h"
#include "detail/RenderTimeline.h"
#include "detail/TrackPreparation.h"
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
#include <gsl-lite/gsl-lite.hpp>

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
#include <unordered_map>
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
    struct StagedPlaybackState;

    struct BackendErrorEvent final
    {
      std::uint64_t generation = 0;
      std::string message;
      std::uint64_t playbackGeneration = 0;
    };

    struct SourceErrorEvent final
    {
      std::uint64_t sourceGeneration = 0;
      std::uint64_t playbackGeneration = 0;
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
      std::uint64_t playbackGeneration = 0;
    };

    struct DrainCompleteEvent final
    {
      std::uint64_t generation = 0;
      std::uint64_t drainEpoch = 0;
      std::uint64_t playbackGeneration = 0;
    };

    struct RouteReadyEvent final
    {
      std::uint64_t generation = 0;
      BackendId backendId;
      std::string routeAnchor;
      std::uint64_t playbackGeneration = 0;
    };

    struct FormatChangedEvent final
    {
      std::uint64_t generation = 0;
      Format format;
      std::uint64_t playbackGeneration = 0;
    };

    struct PropertyChangedEvent final
    {
      std::uint64_t generation = 0;
      PropertySnapshot snapshot;
      std::uint64_t playbackGeneration = 0;
    };

    struct StagedPlaybackState final
    {
      std::uint64_t sourceGeneration = 0;
      std::uint64_t playbackGeneration = 0;
      std::optional<Error> optError = std::nullopt;
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
          gsl_Expects(!isCurrentThread());
          _eventThread.request_stop();
          _eventSignal.release();
          _eventThread.join();
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
    std::atomic<std::uint64_t> currentPlaybackGeneration{1};
    std::atomic<std::uint64_t> callbackGenerationFloor{1};

    // Generation-bearing notifications take this lock across the final
    // generation check and user invocation. Receipt-producing controls install
    // the new floor under controlMutex, then acquire this lock after releasing
    // controlMutex. That lets already-running callbacks finish before the
    // receipt returns without creating a control/callback lock inversion.
    // Recursive entry is required because an Engine callback may synchronously
    // issue another receipt-producing control command.
    mutable std::recursive_mutex callbackDeliveryMutex;

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
    std::uint64_t nextPlaybackGeneration = 2;
    std::uint64_t startContextRevision = 1;
    std::atomic_size_t outstandingPreparedStartCount{0};
    std::unordered_map<std::uint64_t, std::weak_ptr<StagedPlaybackState>> stagedPlaybacks;
    std::optional<PlaybackItem> optCurrentItem;
    Status status;
    OnTrackEnded onTrackEnded;
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

    void startEventWorker()
    {
      eventQueue.start(
        [this] -> std::optional<Notifications>
        {
          auto const lock = std::scoped_lock{controlMutex};

          if (lifecycleState != LifecycleState::Running)
          {
            return std::nullopt;
          }

          auto signal = RtSignal{};

          if (!eventQueue.tryPopRtSignal(signal))
          {
            return std::nullopt;
          }

          return processRtSignal(signal);
        },
        [this](PlaybackEvent& event)
        {
          auto const lock = std::scoped_lock{controlMutex};

          if (lifecycleState != LifecycleState::Running)
          {
            return Notifications{};
          }

          return processPlaybackEvent(event);
        });
    }

    ~Impl()
    {
      shutdown();
      gsl_Expects(outstandingPreparedStartCount.load(std::memory_order_acquire) == 0);
    }

    void shutdown() noexcept
    {
      gsl_Expects(!eventQueue.isCurrentThread());
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

    bool currentTransitionMatches(std::optional<Format> const& optBackendFormat,
                                  std::optional<DecodedStreamInfo> const& optStreamInfo) const
    {
      auto const lock = std::scoped_lock{transitionMutex};
      return optCurrentBackendFormat == optBackendFormat && optCurrentStreamInfo == optStreamInfo;
    }

    void publishPreparedNext(std::unique_ptr<TrackNode> nodePtr)
    {
      gsl_Expects(nodePtr != nullptr);
      gsl_Expects(timeline.lookaheadNode() == nullptr);
      timeline.armLookahead(std::move(nodePtr));
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

    std::optional<PlaybackItem> clearPreparedNextForGeneration(std::uint64_t sourceGeneration)
    {
      waitForSpliceHandoff();
      settlePendingRtSignals();

      auto* const session = timeline.lookaheadNode();

      if (session == nullptr || session->sourceGeneration != sourceGeneration)
      {
        return std::nullopt;
      }

      if (auto* const disarmed = timeline.disarmLookahead(); disarmed == session)
      {
        auto item = session->item;
        timeline.dropDisarmedLookahead(disarmed);
        return item;
      }

      return std::nullopt;
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
            enqueuePlaybackEvent(DrainCompleteEvent{.generation = signal.generation,
                                                    .drainEpoch = signal.drainEpoch,
                                                    .playbackGeneration = signal.playbackGeneration});
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
        case RtSignalKind::Drained:
          return processDrainCompleteSignal(signal.generation, signal.drainEpoch, signal.playbackGeneration);
      }

      return {};
    }

    struct EngineRenderTarget final : public RenderTarget
    {
      EngineRenderTarget(Impl& ownerRef,
                         BackendId backendIdValue,
                         std::uint64_t generationValue,
                         std::uint64_t playbackGenerationValue)
        : owner{ownerRef}
        , backendId{std::move(backendIdValue)}
        , generation{generationValue}
        , playbackGeneration{playbackGenerationValue}
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

        std::ignore = owner.enqueueRtSignal(RtSignal{.kind = RtSignalKind::Drained,
                                                     .generation = generation,
                                                     .drainEpoch = signalDrainEpoch,
                                                     .playbackGeneration = playbackGeneration});
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

            owner.enqueuePlaybackEvent(RouteReadyEvent{.generation = generation,
                                                       .backendId = backendId,
                                                       .routeAnchor = std::string{routeAnchor},
                                                       .playbackGeneration = playbackGeneration});
          });
      }

      void handleFormatChanged(Format const& format) noexcept override
      {
        terminateOnException(
          [&]
          {
            if (owner.isActiveRenderTarget(generation))
            {
              owner.enqueuePlaybackEvent(FormatChangedEvent{
                .generation = generation, .format = format, .playbackGeneration = playbackGeneration});
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

            owner.enqueuePlaybackEvent(PropertyChangedEvent{
              .generation = generation, .snapshot = std::move(snapshot), .playbackGeneration = playbackGeneration});
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

            owner.enqueuePlaybackEvent(BackendErrorEvent{
              .generation = generation, .message = std::string{message}, .playbackGeneration = playbackGeneration});
          });
      }

      Impl& owner;
      BackendId backendId;
      std::uint64_t generation = 0;
      std::uint64_t playbackGeneration = 0;
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

    RenderTarget* createRenderTarget(Backend& backend, std::uint64_t playbackGeneration)
    {
      auto const generation = nextRenderTargetGeneration++;
      renderTargetPtr =
        std::make_unique<EngineRenderTarget>(*this, backend.backendId(), generation, playbackGeneration);
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

    PendingRouteNotifications capturePendingRouteNotifications(std::uint64_t const playbackGeneration)
    {
      return PendingRouteNotifications{
        .stateChanged = onStateChanged,
        .routeChanged = onRouteChanged,
        .routeSnapshot = RouteStatus{
          .state = routeTracker.state(), .optAnchor = routeTracker.anchor(), .generation = playbackGeneration}};
    }

    // ── Playback State Transitions ──────────────────────────────────
    Notifications completeDrain(std::uint64_t generation,
                                std::uint64_t signalDrainEpoch,
                                std::uint64_t playbackGeneration)
    {
      auto onTrackEndedCallback = OnTrackEnded{};
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
      appendRouteNotification(
        notifications, std::move(onRouteChangedCallback), RouteStatus{.generation = playbackGeneration});
      appendTrackEndedNotification(
        notifications, std::move(onTrackEndedCallback), TrackEnded{.generation = playbackGeneration});

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

    void appendRouteNotification(Notifications& notifications, OnRouteChanged callback, RouteStatus snapshot)
    {
      if (callback)
      {
        notifications.emplace_back(
          [this, callback = std::move(callback), snapshot = std::move(snapshot)]
          {
            auto const deliveryLock = std::scoped_lock{callbackDeliveryMutex};

            if (snapshot.generation < callbackGenerationFloor.load(std::memory_order_acquire))
            {
              return;
            }

            callback(snapshot);
          });
      }
    }

    void appendPendingRouteNotifications(Notifications& notifications, PendingRouteNotifications pending)
    {
      appendStateChangedNotification(notifications, std::move(pending.stateChanged));
      appendRouteNotification(notifications, std::move(pending.routeChanged), std::move(pending.routeSnapshot));
    }

    void appendTrackEndedNotification(Notifications& notifications, OnTrackEnded callback, TrackEnded event)
    {
      if (callback)
      {
        notifications.emplace_back(
          [this, callback = std::move(callback), event]
          {
            auto const deliveryLock = std::scoped_lock{callbackDeliveryMutex};

            if (event.generation < callbackGenerationFloor.load(std::memory_order_acquire))
            {
              return;
            }

            callback(event);
          });
      }
    }

    void appendTrackAdvancedNotification(Notifications& notifications, OnTrackAdvanced callback, TrackAdvanced event)
    {
      if (callback)
      {
        notifications.emplace_back(
          [this, callback = std::move(callback), event = std::move(event)]
          {
            auto const deliveryLock = std::scoped_lock{callbackDeliveryMutex};

            if (event.generation < callbackGenerationFloor.load(std::memory_order_acquire))
            {
              return;
            }

            callback(event);
          });
      }
    }

    void appendPlaybackFailureNotification(Notifications& notifications,
                                           OnPlaybackFailure callback,
                                           PlaybackFailure failure)
    {
      if (callback)
      {
        notifications.emplace_back(
          [this, callback = std::move(callback), failure = std::move(failure)]
          {
            auto const deliveryLock = std::scoped_lock{callbackDeliveryMutex};

            if (failure.generation < callbackGenerationFloor.load(std::memory_order_acquire))
            {
              return;
            }

            callback(failure);
          });
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
        routeSnapshot = RouteStatus{
          .state = routeTracker.state(), .optAnchor = routeTracker.anchor(), .generation = session->playbackGeneration};
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

      appendTrackAdvancedNotification(
        notifications,
        std::move(trackAdvanced),
        TrackAdvanced{
          .itemId = session->item.id, .input = session->item.input, .generation = session->playbackGeneration});
      appendStateChangedNotification(notifications, std::move(stateChanged));
      appendRouteNotification(notifications, std::move(routeChanged), std::move(routeSnapshot));
      return notifications;
    }

    Notifications processDrainCompleteSignal(std::uint64_t generation,
                                             std::uint64_t signalDrainEpoch,
                                             std::uint64_t playbackGeneration)
    {
      return completeDrain(generation, signalDrainEpoch, playbackGeneration);
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
                                            .generation = event.playbackGeneration,
                                            .error = Error{.code = Error::Code::IoError, .message = event.message},
                                            .recoverable = false,
                                          });
      }

      appendStateChangedNotification(notifications, std::move(stateChanged));
      return notifications;
    }

    Notifications processSourceErrorEvent(SourceErrorEvent const& event)
    {
      if (auto const it = stagedPlaybacks.find(event.sourceGeneration); it != stagedPlaybacks.end())
      {
        auto const stagedStatePtr = it->second.lock();
        stagedPlaybacks.erase(it);

        if (stagedStatePtr && stagedStatePtr->playbackGeneration == event.playbackGeneration)
        {
          if (!stagedStatePtr->optError)
          {
            stagedStatePtr->optError = event.error;
          }

          auto stateChanged = std::function<void()>{};
          {
            auto const lock = std::scoped_lock{stateMutex};
            stateChanged = onStateChanged;
          }

          auto notifications = Notifications{};
          appendStateChangedNotification(notifications, std::move(stateChanged));
          return notifications;
        }
      }

      if (auto const optPreparedItem = clearPreparedNextForGeneration(event.sourceGeneration); optPreparedItem)
      {
        auto failureCallback = OnPlaybackFailure{};
        {
          auto const lock = std::scoped_lock{stateMutex};
          failureCallback = onPlaybackFailure;
        }

        auto notifications = Notifications{};
        auto failureError = event.error;

        if (failureError.message.empty())
        {
          failureError.message = "Prepared PCM source failed";
        }

        appendPlaybackFailureNotification(notifications,
                                          std::move(failureCallback),
                                          PlaybackFailure{
                                            .kind = PlaybackFailureKind::Decode,
                                            .itemId = optPreparedItem->id,
                                            .input = optPreparedItem->input,
                                            .generation = event.playbackGeneration,
                                            .error = std::move(failureError),
                                            .recoverable = true,
                                          });
        return notifications;
      }

      auto const message = event.error.message.empty() ? std::string{"PCM source failed"} : event.error.message;
      auto endedCallback = OnTrackEnded{};
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
                                            .generation = event.playbackGeneration,
                                            .error = std::move(failureError),
                                            .recoverable = true,
                                          });
      }

      appendStateChangedNotification(notifications, std::move(stateChanged));

      appendTrackEndedNotification(
        notifications, std::move(endedCallback), TrackEnded{.generation = event.playbackGeneration});

      return notifications;
    }

    Notifications processDrainCompleteEvent(DrainCompleteEvent const& event)
    {
      return completeDrain(event.generation, event.drainEpoch, event.playbackGeneration);
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
        pendingNotifications = capturePendingRouteNotifications(event.playbackGeneration);
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
        pendingNotifications = capturePendingRouteNotifications(event.playbackGeneration);
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

        pendingNotifications = capturePendingRouteNotifications(event.playbackGeneration);
      }

      appendPendingRouteNotifications(notifications, std::move(pendingNotifications));
      return notifications;
    }

    // ── Track opening ──────────────────────────────────────────────
    detail::TrackSession::OnSourceErrorFn makeSourceErrorHandler(std::uint64_t const sourceGeneration,
                                                                 std::uint64_t const playbackGeneration)
    {
      return [this, sourceGeneration, playbackGeneration](Error const& error)
      {
        enqueuePlaybackEvent(SourceErrorEvent{
          .sourceGeneration = sourceGeneration, .playbackGeneration = playbackGeneration, .error = error});
      };
    }

    static TrackNode makeTrackNode(PlaybackItem const& item,
                                   detail::TrackSession::OpenedTrack session,
                                   std::uint64_t const sourceGeneration,
                                   std::uint64_t const playbackGeneration)
    {
      return TrackNode{.item = item,
                       .sourcePtr = std::move(session.sourcePtr),
                       .backendFormat = session.backendFormat,
                       .info = session.info,
                       .sourceGeneration = sourceGeneration,
                       .playbackGeneration = playbackGeneration};
    }

    Result<TrackNode> openTrackSession(PlaybackItem const& item,
                                       std::uint64_t sourceGeneration,
                                       std::uint64_t playbackGeneration,
                                       std::chrono::milliseconds initialOffset = {})
    {
      auto prepared = detail::TrackSession::prepare(
        item.input, currentDevice, backendPtr->backendId(), backendPtr->profileId(), decoderFactory, initialOffset);

      if (!prepared)
      {
        return std::unexpected{prepared.error()};
      }

      auto session = detail::TrackSession::activate(
        std::move(*prepared), makeSourceErrorHandler(sourceGeneration, playbackGeneration));

      if (!session)
      {
        return std::unexpected{session.error()};
      }

      return makeTrackNode(item, std::move(*session), sourceGeneration, playbackGeneration);
    }

    void registerStagedPlayback(std::shared_ptr<StagedPlaybackState> const& stagedStatePtr)
    {
      assert(stagedStatePtr != nullptr);
      stagedPlaybacks.insert_or_assign(stagedStatePtr->sourceGeneration, stagedStatePtr);
      outstandingPreparedStartCount.fetch_add(1, std::memory_order_acq_rel);
    }

    void unregisterStagedPlaybackUnlocked(std::shared_ptr<StagedPlaybackState> const& stagedStatePtr)
    {
      if (!stagedStatePtr)
      {
        return;
      }

      auto const it = stagedPlaybacks.find(stagedStatePtr->sourceGeneration);

      if (it == stagedPlaybacks.end())
      {
        return;
      }

      if (auto const registeredStatePtr = it->second.lock();
          !registeredStatePtr || registeredStatePtr == stagedStatePtr)
      {
        stagedPlaybacks.erase(it);
      }
    }

    void unregisterStagedPlayback(std::shared_ptr<StagedPlaybackState> const& stagedStatePtr)
    {
      auto const lock = std::scoped_lock{controlMutex};
      releaseStagedPlaybackRegistrationUnlocked(stagedStatePtr);
    }

    void releaseStagedPlaybackRegistrationUnlocked(std::shared_ptr<StagedPlaybackState> const& stagedStatePtr)
    {
      unregisterStagedPlaybackUnlocked(stagedStatePtr);
      gsl_Expects(outstandingPreparedStartCount.load(std::memory_order_acquire) != 0);
      outstandingPreparedStartCount.fetch_sub(1, std::memory_order_acq_rel);
    }

    void publishCurrentTrackState(TrackNode const& session)
    {
      // Track preparation and activation ran lock-free above; only the
      // status/routeTracker publication needs the lock, which status() also
      // takes when it reads them concurrently from the UI thread.
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

    std::uint64_t reservePlaybackGeneration() noexcept { return nextPlaybackGeneration++; }

    void acceptPlaybackGeneration(std::uint64_t generation) noexcept
    {
      currentPlaybackGeneration.store(generation, std::memory_order_release);
      callbackGenerationFloor.store(generation, std::memory_order_release);
    }

    void synchronizeCallbackBarrier() const { auto const deliveryLock = std::scoped_lock{callbackDeliveryMutex}; }

    void setBackendUnlocked(std::unique_ptr<Backend> nextBackendPtr, Device const& device);
    void updateDeviceUnlocked(Device const& device);
    Result<> applyInitialOffset(TrackNode& node, std::chrono::milliseconds initialOffset);
    void playUnlocked(PlaybackItem const& item, std::chrono::milliseconds initialOffset = {});
    std::optional<PlaybackItemId> clearNextUnlocked();
    void pauseUnlocked();
    void resumeUnlocked();
    void stopPlaybackUnlocked();
    void stopUnlocked();
    void commitPreparedPlaybackUnlocked(std::unique_ptr<TrackNode> nodePtr,
                                        std::chrono::milliseconds initialOffset,
                                        std::uint64_t playbackGeneration);
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

  struct Engine::PreparedPlaybackStart::Impl final
  {
    Impl() = default;
    Impl(Impl const&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl const&) = delete;
    Impl& operator=(Impl&&) = delete;

    ~Impl()
    {
      if (stagedRegistrationActive && owner != nullptr)
      {
        owner->unregisterStagedPlayback(stagedStatePtr);
      }
    }

    Engine::Impl* owner = nullptr;
    std::unique_ptr<Engine::Impl::TrackNode> nodePtr;
    std::shared_ptr<Engine::Impl::StagedPlaybackState> stagedStatePtr;
    std::chrono::milliseconds initialOffset{0};
    std::uint64_t baseGeneration = 0;
    std::uint64_t candidateGeneration = 0;
    std::uint64_t startContextRevision = 0;
    bool stagedRegistrationActive = false;
  };

  Engine::PreparedPlaybackStart::PreparedPlaybackStart(std::unique_ptr<PreparedPlaybackStart::Impl> implPtr)
    : _implPtr{std::move(implPtr)}
  {
  }

  Engine::PreparedPlaybackStart::~PreparedPlaybackStart() = default;
  Engine::PreparedPlaybackStart::PreparedPlaybackStart(PreparedPlaybackStart&&) noexcept = default;
  Engine::PreparedPlaybackStart& Engine::PreparedPlaybackStart::operator=(PreparedPlaybackStart&&) noexcept = default;

  struct detail::TrackPreparation::Impl final
  {
    Engine::PlaybackItem item;
    Device device;
    BackendId backendId;
    ProfileId profileId;
    DecoderFactoryFn decoderFactory;
    std::chrono::milliseconds initialOffset{0};
    std::uint64_t basePlaybackGeneration = 0;
    std::uint64_t startContextRevision = 0;
    Purpose purpose = Purpose::ExplicitStart;
    std::optional<Format> optCurrentBackendFormat;
    std::optional<DecodedStreamInfo> optCurrentStreamInfo;
    std::optional<detail::TrackSession::PreparedTrack> optPreparedTrack;
    bool logicalDrainFallback = false;
    bool preparationAttempted = false;
  };

  detail::TrackPreparation::TrackPreparation(std::unique_ptr<Impl> implPtr)
    : _implPtr{std::move(implPtr)}
  {
  }

  detail::TrackPreparation::~TrackPreparation() = default;
  detail::TrackPreparation::TrackPreparation(TrackPreparation&&) noexcept = default;
  detail::TrackPreparation& detail::TrackPreparation::operator=(TrackPreparation&&) noexcept = default;

  bool detail::TrackPreparation::requiresWorker() const noexcept
  {
    return _implPtr != nullptr && !_implPtr->logicalDrainFallback;
  }

  bool detail::TrackPreparation::matchesControlContext(Engine const& engine,
                                                       std::uint64_t const currentGeneration) const
  {
    return _implPtr != nullptr && _implPtr->basePlaybackGeneration == currentGeneration &&
           _implPtr->startContextRevision == engine._implPtr->startContextRevision &&
           _implPtr->device == engine._implPtr->currentDevice &&
           _implPtr->backendId == engine._implPtr->backendPtr->backendId() &&
           _implPtr->profileId == engine._implPtr->backendPtr->profileId();
  }

  Result<> detail::TrackPreparation::prepare()
  {
    if (!_implPtr || _implPtr->preparationAttempted)
    {
      return makeError(Error::Code::InvalidState, "Track preparation may only run once");
    }

    _implPtr->preparationAttempted = true;

    if (_implPtr->logicalDrainFallback)
    {
      return {};
    }

    try
    {
      auto prepared = detail::TrackSession::prepare(_implPtr->item.input,
                                                    _implPtr->device,
                                                    _implPtr->backendId,
                                                    _implPtr->profileId,
                                                    _implPtr->decoderFactory,
                                                    _implPtr->initialOffset);

      if (!prepared)
      {
        return std::unexpected{prepared.error()};
      }

      _implPtr->optPreparedTrack.emplace(std::move(*prepared));
      return {};
    }
    catch (std::exception const& error)
    {
      return makeError(Error::Code::Generic, error.what());
    }
    catch (...)
    {
      return makeError(Error::Code::Generic, "Unknown failure during track preparation");
    }
  }

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

    ++startContextRevision;
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
    if (device == currentDevice)
    {
      return;
    }

    ++startContextRevision;
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
    auto const playbackGeneration = reservePlaybackGeneration();
    acceptPlaybackGeneration(playbackGeneration);
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

    auto openedTrack = openTrackSession(item, sourceGeneration, playbackGeneration);

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
                                            .generation = playbackGeneration,
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
        markRouteActivationFailureUnlocked(notifications, item, playbackGeneration, seekResult.error());
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
    auto* renderTarget = createRenderTarget(*backendPtr, playbackGeneration);

    if (auto const openResult = backendPtr->open(currentNode->backendFormat, renderTarget); !openResult)
    {
      retireRenderTarget();
      backendPtr->close();
      resetRenderTarget();
      auto notifications = Notifications{};
      {
        auto const lock = std::scoped_lock{stateMutex};
        markRouteActivationFailureUnlocked(notifications, item, playbackGeneration, openResult.error());
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

  void Engine::Impl::commitPreparedPlaybackUnlocked(std::unique_ptr<TrackNode> nodePtr,
                                                    std::chrono::milliseconds const initialOffset,
                                                    std::uint64_t const playbackGeneration)
  {
    assert(nodePtr != nullptr);
    assert(nodePtr->playbackGeneration == playbackGeneration);

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

    {
      auto const lock = std::scoped_lock{stateMutex};
      underrunCount = 0;
      routeTracker.clear();
      backendStarted = false;
      cancelPendingDrainSignal();
      status.transport = Transport::Opening;
      optCurrentItem = nodePtr->item;
      syncBackendIdentity();
    }

    publishCurrentTransitionState(*nodePtr);
    publishCurrentTrackState(*nodePtr);
    accumulatedFrames.store(
      durationToSamples(initialOffset, nodePtr->info.outputFormat.sampleRate), std::memory_order_relaxed);

    {
      auto const lock = std::scoped_lock{stateMutex};
      status.elapsed = initialOffset;
      status.transport = Transport::Buffering;
    }

    auto* const currentNode = nodePtr.get();
    auto const item = currentNode->item;
    publishCurrentNode(std::move(nodePtr));
    auto* const renderTarget = createRenderTarget(*backendPtr, playbackGeneration);

    if (auto const openResult = backendPtr->open(currentNode->backendFormat, renderTarget); !openResult)
    {
      retireRenderTarget();
      backendPtr->close();
      resetRenderTarget();
      auto notifications = Notifications{};
      {
        auto const lock = std::scoped_lock{stateMutex};
        markRouteActivationFailureUnlocked(notifications, item, playbackGeneration, openResult.error());
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

      if (status.transport == Transport::Error)
      {
        return;
      }

      status.transport = Transport::Playing;
      backendStarted = true;
    }

    backendPtr->start();
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

  void Engine::Impl::stopPlaybackUnlocked()
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

  void Engine::Impl::stopUnlocked()
  {
    acceptPlaybackGeneration(reservePlaybackGeneration());
    stopPlaybackUnlocked();
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
    : _implPtr{std::make_unique<Impl>(std::move(backendPtr), device, std::move(decoderFactory))}
  {
    _implPtr->syncBackendStatus();
    _implPtr->startEventWorker();
  }

  Engine::~Engine()
  {
    gsl_Expects(_implPtr != nullptr);
    gsl_Expects(_implPtr->outstandingPreparedStartCount.load(std::memory_order_acquire) == 0);
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
    bool applied = false;
    {
      auto const controlLock = _implPtr->lockControl();

      if (!controlLock)
      {
        return;
      }

      _implPtr->setBackendUnlocked(std::move(backendPtr), device);
      applied = true;
    }

    if (applied)
    {
      _implPtr->synchronizeCallbackBarrier();
    }
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

  void Engine::setOnTrackEnded(OnTrackEnded callback)
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
    return RouteStatus{.state = _implPtr->routeTracker.state(),
                       .optAnchor = _implPtr->routeTracker.anchor(),
                       .generation = _implPtr->currentPlaybackGeneration.load(std::memory_order_acquire)};
  }

  Result<detail::TrackPreparation> detail::TrackPreparation::capture(Engine& engine,
                                                                     Engine::PlaybackItem const& item,
                                                                     std::chrono::milliseconds const initialOffset,
                                                                     Purpose const purpose)
  {
    auto const controlLock = engine._implPtr->lockControl();

    if (!controlLock)
    {
      return makeError(Error::Code::InvalidState, "Engine is shut down");
    }

    return captureUnlocked(engine, item, initialOffset, purpose);
  }

  Result<detail::TrackPreparation> detail::TrackPreparation::captureUnlocked(
    Engine& engine,
    Engine::PlaybackItem const& item,
    std::chrono::milliseconds const initialOffset,
    Purpose const purpose)
  {
    auto preparationImplPtr = std::make_unique<Impl>();
    preparationImplPtr->item = item;
    preparationImplPtr->device = engine._implPtr->currentDevice;
    preparationImplPtr->backendId = engine._implPtr->backendPtr->backendId();
    preparationImplPtr->profileId = engine._implPtr->backendPtr->profileId();
    preparationImplPtr->decoderFactory = engine._implPtr->decoderFactory;
    preparationImplPtr->initialOffset = initialOffset;
    preparationImplPtr->basePlaybackGeneration =
      engine._implPtr->currentPlaybackGeneration.load(std::memory_order_acquire);
    preparationImplPtr->startContextRevision = engine._implPtr->startContextRevision;
    preparationImplPtr->purpose = purpose;

    if (purpose == Purpose::GaplessLookahead)
    {
      auto const transitionLock = std::scoped_lock{engine._implPtr->transitionMutex};

      if (!engine._implPtr->optCurrentBackendFormat || !engine._implPtr->optCurrentStreamInfo)
      {
        return makeError(Error::Code::InvalidState, "No active playback to prepare next track");
      }

      preparationImplPtr->optCurrentBackendFormat = engine._implPtr->optCurrentBackendFormat;
      preparationImplPtr->optCurrentStreamInfo = engine._implPtr->optCurrentStreamInfo;
      preparationImplPtr->logicalDrainFallback =
        !Engine::Impl::isGaplessCapable(*preparationImplPtr->optCurrentStreamInfo);
    }

    return TrackPreparation{std::move(preparationImplPtr)};
  }

  Result<Engine::PreparedPlaybackStart> detail::TrackPreparation::adoptStart(Engine& engine) &&
  {
    auto const controlLock = engine._implPtr->lockControl();

    if (!controlLock)
    {
      return makeError(Error::Code::InvalidState, "Engine is shut down");
    }

    return std::move(*this).adoptStartUnlocked(engine);
  }

  Result<Engine::PreparedPlaybackStart> detail::TrackPreparation::adoptStartUnlocked(Engine& engine) &&
  {
    auto consumedPreparation = std::move(*this);
    auto* const preparationImpl = consumedPreparation._implPtr.get();

    if (preparationImpl == nullptr || preparationImpl->purpose != Purpose::ExplicitStart ||
        !preparationImpl->preparationAttempted || !preparationImpl->optPreparedTrack)
    {
      return makeError(Error::Code::InvalidState, "Explicit playback preparation is incomplete");
    }

    auto const currentGeneration = engine._implPtr->currentPlaybackGeneration.load(std::memory_order_acquire);

    if (!consumedPreparation.matchesControlContext(engine, currentGeneration))
    {
      return makeError(Error::Code::Conflict, "Playback or output route changed during track preparation");
    }

    auto const candidateGeneration = engine._implPtr->reservePlaybackGeneration();
    auto const sourceGeneration = engine._implPtr->nextSourceGeneration++;
    using EngineImpl = Engine::Impl;
    auto stagedStatePtr = std::make_shared<EngineImpl::StagedPlaybackState>(EngineImpl::StagedPlaybackState{
      .sourceGeneration = sourceGeneration,
      .playbackGeneration = candidateGeneration,
    });

    class [[nodiscard]] StagedRegistrationGuard final
    {
    public:
      StagedRegistrationGuard(EngineImpl& owner, std::shared_ptr<EngineImpl::StagedPlaybackState> statePtr)
        : _owner{owner}, _statePtr{std::move(statePtr)}, _active{true}
      {
        _owner.registerStagedPlayback(_statePtr);
      }

      ~StagedRegistrationGuard()
      {
        if (_active)
        {
          _owner.releaseStagedPlaybackRegistrationUnlocked(_statePtr);
        }
      }

      void dismiss() noexcept { _active = false; }

      StagedRegistrationGuard(StagedRegistrationGuard const&) = delete;
      StagedRegistrationGuard& operator=(StagedRegistrationGuard const&) = delete;
      StagedRegistrationGuard(StagedRegistrationGuard&&) = delete;
      StagedRegistrationGuard& operator=(StagedRegistrationGuard&&) = delete;

    private:
      EngineImpl& _owner;
      std::shared_ptr<EngineImpl::StagedPlaybackState> _statePtr;
      bool _active = false;
    };

    auto registration = StagedRegistrationGuard{*engine._implPtr, stagedStatePtr};

    try
    {
      auto activated =
        detail::TrackSession::activate(std::move(*preparationImpl->optPreparedTrack),
                                       engine._implPtr->makeSourceErrorHandler(sourceGeneration, candidateGeneration));
      preparationImpl->optPreparedTrack.reset();

      if (!activated)
      {
        return std::unexpected{activated.error()};
      }

      auto preparedImplPtr = std::make_unique<Engine::PreparedPlaybackStart::Impl>();
      preparedImplPtr->owner = engine._implPtr.get();
      preparedImplPtr->nodePtr = std::make_unique<EngineImpl::TrackNode>(
        EngineImpl::makeTrackNode(preparationImpl->item, std::move(*activated), sourceGeneration, candidateGeneration));
      preparedImplPtr->stagedStatePtr = std::move(stagedStatePtr);
      preparedImplPtr->initialOffset = preparationImpl->initialOffset;
      preparedImplPtr->baseGeneration = preparationImpl->basePlaybackGeneration;
      preparedImplPtr->candidateGeneration = candidateGeneration;
      preparedImplPtr->startContextRevision = preparationImpl->startContextRevision;
      preparedImplPtr->stagedRegistrationActive = true;
      registration.dismiss();
      return Engine::PreparedPlaybackStart{std::move(preparedImplPtr)};
    }
    catch (std::exception const& error)
    {
      return makeError(Error::Code::Generic, error.what());
    }
    catch (...)
    {
      return makeError(Error::Code::Generic, "Unknown failure during track activation");
    }
  }

  Result<Engine::PreparedNextResult> detail::TrackPreparation::adoptNext(Engine& engine) &&
  {
    auto const controlLock = engine._implPtr->lockControl();

    if (!controlLock)
    {
      return makeError(Error::Code::InvalidState, "Engine is shut down");
    }

    return std::move(*this).adoptNextUnlocked(engine);
  }

  Result<Engine::PreparedNextResult> detail::TrackPreparation::adoptNextUnlocked(Engine& engine) &&
  {
    auto consumedPreparation = std::move(*this);
    auto* const preparationImpl = consumedPreparation._implPtr.get();

    if (preparationImpl == nullptr || preparationImpl->purpose != Purpose::GaplessLookahead ||
        !preparationImpl->preparationAttempted)
    {
      return makeError(Error::Code::InvalidState, "Lookahead preparation is incomplete");
    }

    auto const currentGeneration = engine._implPtr->currentPlaybackGeneration.load(std::memory_order_acquire);
    auto const currentMatches = engine._implPtr->currentTransitionMatches(
      preparationImpl->optCurrentBackendFormat, preparationImpl->optCurrentStreamInfo);

    if (!consumedPreparation.matchesControlContext(engine, currentGeneration) || !currentMatches)
    {
      return makeError(Error::Code::Conflict, "Playback or output route changed during lookahead preparation");
    }

    if (!preparationImpl->logicalDrainFallback && !preparationImpl->optPreparedTrack)
    {
      return makeError(Error::Code::InvalidState, "Lookahead source preparation is missing");
    }

    bool capable = false;
    auto nodePtr = std::unique_ptr<Engine::Impl::TrackNode>{};

    if (preparationImpl->optPreparedTrack)
    {
      auto const& preparedTrack = *preparationImpl->optPreparedTrack;
      capable = Engine::Impl::isGaplessCapable(preparedTrack.info) &&
                preparationImpl->optCurrentBackendFormat == preparedTrack.backendFormat;

      if (capable)
      {
        auto const sourceGeneration = engine._implPtr->nextSourceGeneration++;

        try
        {
          auto activated = detail::TrackSession::activate(
            std::move(*preparationImpl->optPreparedTrack),
            engine._implPtr->makeSourceErrorHandler(sourceGeneration, currentGeneration));
          preparationImpl->optPreparedTrack.reset();

          if (!activated)
          {
            return std::unexpected{activated.error()};
          }

          nodePtr = std::make_unique<Engine::Impl::TrackNode>(Engine::Impl::makeTrackNode(
            preparationImpl->item, std::move(*activated), sourceGeneration, currentGeneration));
        }
        catch (std::exception const& error)
        {
          return makeError(Error::Code::Generic, error.what());
        }
        catch (...)
        {
          return makeError(Error::Code::Generic, "Unknown failure during lookahead activation");
        }
      }
    }

    // Once the old cursor is disarmed, clearPreparedNext settles a splice that
    // raced adoption. With no armed lookahead left, the refreshed transition
    // snapshot cannot change again before this control command publishes.
    engine._implPtr->clearPreparedNext();

    if (!engine._implPtr->currentTransitionMatches(
          preparationImpl->optCurrentBackendFormat, preparationImpl->optCurrentStreamInfo))
    {
      return makeError(Error::Code::Conflict, "Playback changed while adopting lookahead preparation");
    }

    if (!capable)
    {
      return Engine::PreparedNextResult{.itemId = preparationImpl->item.id,
                                        .transition = Engine::PreparedTransitionMode::DrainFallback,
                                        .generation = currentGeneration};
    }

    engine._implPtr->publishPreparedNext(std::move(nodePtr));
    return Engine::PreparedNextResult{.itemId = preparationImpl->item.id,
                                      .transition = Engine::PreparedTransitionMode::Gapless,
                                      .generation = currentGeneration};
  }

  Result<Engine::PreparedPlaybackStart> Engine::stagePlayback(PlaybackItem const& item,
                                                              std::chrono::milliseconds const initialOffset)
  {
    auto const controlLock = _implPtr->lockControl();

    if (!controlLock)
    {
      return makeError(Error::Code::InvalidState, "Engine is shut down");
    }

    auto preparation = detail::TrackPreparation::captureUnlocked(
      *this, item, initialOffset, detail::TrackPreparation::Purpose::ExplicitStart);

    if (!preparation)
    {
      return std::unexpected{preparation.error()};
    }

    if (auto prepared = preparation->prepare(); !prepared)
    {
      return std::unexpected{prepared.error()};
    }

    return std::move(*preparation).adoptStartUnlocked(*this);
  }

  Result<Engine::PlaybackStartReceipt> Engine::commitPlayback(PreparedPlaybackStart&& preparedStart)
  {
    auto stagedStart = std::move(preparedStart);
    auto receipt = PlaybackStartReceipt{};
    {
      auto const controlLock = _implPtr->lockControl();

      if (!controlLock)
      {
        return makeError(Error::Code::InvalidState, "Engine is shut down");
      }

      auto* const preparedImpl = stagedStart._implPtr.get();

      if (preparedImpl == nullptr || preparedImpl->owner != _implPtr.get() || !preparedImpl->nodePtr)
      {
        return makeError(Error::Code::InvalidState, "Prepared playback belongs to a different engine");
      }

      auto const currentGeneration = _implPtr->currentPlaybackGeneration.load(std::memory_order_acquire);

      if (preparedImpl->baseGeneration != currentGeneration ||
          preparedImpl->startContextRevision != _implPtr->startContextRevision)
      {
        return makeError(Error::Code::Conflict, "Playback changed after this start was staged");
      }

      if (preparedImpl->stagedStatePtr && preparedImpl->stagedStatePtr->optError)
      {
        return std::unexpected{*preparedImpl->stagedStatePtr->optError};
      }

      auto const itemId = preparedImpl->nodePtr->item.id;
      auto const candidateGeneration = preparedImpl->candidateGeneration;
      auto const initialOffset = preparedImpl->initialOffset;
      _implPtr->releaseStagedPlaybackRegistrationUnlocked(preparedImpl->stagedStatePtr);
      preparedImpl->stagedRegistrationActive = false;
      _implPtr->acceptPlaybackGeneration(candidateGeneration);
      _implPtr->commitPreparedPlaybackUnlocked(std::move(preparedImpl->nodePtr), initialOffset, candidateGeneration);
      receipt = PlaybackStartReceipt{
        .itemId = itemId,
        .generation = candidateGeneration,
        .cancellationBarrier = PreparedCancellationBarrier{.generation = candidateGeneration},
      };
      stagedStart._implPtr.reset();
    }

    _implPtr->synchronizeCallbackBarrier();
    return receipt;
  }

  Engine::PreparedCancellationBarrier Engine::stopWithBarrier()
  {
    auto generation = _implPtr->currentPlaybackGeneration.load(std::memory_order_acquire);
    bool stopped = false;
    {
      auto const controlLock = _implPtr->lockControl();

      if (!controlLock)
      {
        return PreparedCancellationBarrier{.generation = generation};
      }

      _implPtr->stopUnlocked();
      generation = _implPtr->currentPlaybackGeneration.load(std::memory_order_acquire);
      stopped = true;
    }

    if (stopped)
    {
      _implPtr->synchronizeCallbackBarrier();
    }

    return PreparedCancellationBarrier{.generation = generation};
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

  std::uint64_t Engine::playbackGeneration() const noexcept
  {
    return _implPtr->currentPlaybackGeneration.load(std::memory_order_acquire);
  }

  Engine::Status Engine::status() const
  {
    // Unlike scalar state-only queries, the complete snapshot observes the
    // current source's PCM queue. Serialize that observation with seek(), whose
    // control-ordered source reset requires exclusive queue access.
    auto const controlLock = std::scoped_lock{_implPtr->controlMutex};
    auto const sourcePtr = _implPtr->currentSource();
    auto snap = Status{};
    {
      auto const stateLock = std::scoped_lock{_implPtr->stateMutex};
      snap = _implPtr->status;
      snap.routeState = _implPtr->routeTracker.state();
    }

    auto const totalFrames = _implPtr->accumulatedFrames.load(std::memory_order_relaxed);
    auto const sampleRate = _implPtr->engineSampleRate.load(std::memory_order_relaxed);
    snap.elapsed = samplesToDuration(totalFrames, sampleRate);
    snap.bufferedDuration = sourcePtr ? sourcePtr->bufferedDuration() : std::chrono::milliseconds{0};
    snap.underrunCount = _implPtr->underrunCount.load(std::memory_order_relaxed);
    return snap;
  }

  void Engine::play(PlaybackItem const& item, std::chrono::milliseconds const initialOffset)
  {
    bool played = false;
    {
      auto const controlLock = _implPtr->lockControl();

      if (!controlLock)
      {
        return;
      }

      _implPtr->playUnlocked(item, initialOffset);
      played = true;
    }

    if (played)
    {
      _implPtr->synchronizeCallbackBarrier();
    }
  }

  Result<Engine::PreparedNextResult> Engine::setNext(PlaybackItem const& item)
  {
    auto const controlLock = _implPtr->lockControl();

    if (!controlLock)
    {
      return makeError(Error::Code::InvalidState, "Engine is shut down");
    }

    // Settle a splice already claimed by render before capturing the current
    // transition. Adoption repeats this step to cover a splice that races the
    // synchronous decoder open.
    _implPtr->clearPreparedNext();

    auto preparation =
      detail::TrackPreparation::captureUnlocked(*this, item, {}, detail::TrackPreparation::Purpose::GaplessLookahead);

    if (!preparation)
    {
      return std::unexpected{preparation.error()};
    }

    if (auto prepared = preparation->prepare(); !prepared)
    {
      return std::unexpected{prepared.error()};
    }

    return std::move(*preparation).adoptNextUnlocked(*this);
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
    std::ignore = stopWithBarrier();
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
