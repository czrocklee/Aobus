// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "detail/RenderPath.h"
#include "detail/RenderTimeline.h"
#include "detail/TrackSession.h"
#include <ao/AudioCodec.h>
#include <ao/Error.h>
#include <ao/audio/Backend.h>
#include <ao/audio/DecoderTypes.h>
#include <ao/audio/Engine.h>
#include <ao/audio/Format.h>
#include <ao/audio/IBackend.h>
#include <ao/audio/IRenderTarget.h>
#include <ao/audio/ISource.h>
#include <ao/audio/Property.h>
#include <ao/audio/Transport.h>
#include <ao/audio/detail/RouteTracker.h>

#include <boost/lockfree/policies.hpp>
#include <boost/lockfree/spsc_queue.hpp>

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
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
  } // namespace

  // ── Engine::Impl: data bucket + callbacks + handlers ────────────

  struct Engine::Impl final
  {
    struct RenderSession;
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
      PropertyId id{};
      std::optional<PropertyValue> optValue;
      PropertyInfo volumeInfo;
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

    Device currentDevice;
    RenderTimeline timeline;

    // Serializes external control commands that coordinate backend lifecycle,
    // source ownership, and status publication. Backend callbacks deliberately
    // do not take this lock; they may run on the render thread while stop()
    // waits for that thread to quiesce.
    mutable std::mutex controlMutex;

    class [[nodiscard]] ControlLock final
    {
    public:
      explicit ControlLock(Impl& owner)
        : _lock{owner.controlMutex}
      {
        owner.waitForSpliceHandoff();
        owner.settlePendingRtSignals();
      }

      ~ControlLock();

      ControlLock(ControlLock const&) = delete;
      ControlLock& operator=(ControlLock const&) = delete;
      ControlLock(ControlLock&&) = delete;
      ControlLock& operator=(ControlLock&&) = delete;

    private:
      std::scoped_lock<std::mutex> _lock;
    };

    std::atomic<bool> backendStarted{false};
    std::atomic<bool> playbackDrainPending{false};
    std::atomic<std::uint64_t> drainEpoch{1};
    std::atomic<std::uint32_t> underrunCount{0};
    std::atomic<std::uint64_t> accumulatedFrames{0};
    std::atomic<std::uint32_t> engineSampleRate{0};
    std::atomic<std::uint32_t> engineFrameBytes{0};
    std::atomic<std::uint64_t> activeRenderGeneration{0};

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
    std::uint64_t nextRenderGeneration = 1;
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
    std::unique_ptr<RenderSession> renderSessionPtr;

    // The event thread is woken by a counting semaphore rather than a condition
    // variable so the render thread can signal it without a mutex (a mutex-free
    // release avoids the lost-wakeup a lock-free ring would otherwise have with a
    // condition variable). Every producer releases once per item; the consumer
    // drains both the wait-free rtSignalRing and the mutex-guarded eventQueue.
    static constexpr std::size_t kRtSignalCapacity = 64;
    mutable std::mutex eventMutex;
    std::deque<PlaybackEvent> eventQueue;
    boost::lockfree::spsc_queue<RtSignal, boost::lockfree::capacity<kRtSignalCapacity>> rtSignalRing;
    std::counting_semaphore<> eventSignal{0};
    std::jthread eventThread;

    // Must be declared last so the PipeWire thread loop is stopped
    // before the callbacks and state it accesses are destroyed.
    std::unique_ptr<IBackend> backendPtr;

    // ── Construction & Destruction ────────────────────────────────
    Impl(std::unique_ptr<IBackend> backendPtr, Device device, DecoderFactoryFn decoderFactory)
      : currentDevice{std::move(device)}, decoderFactory{std::move(decoderFactory)}, backendPtr{std::move(backendPtr)}
    {
      syncBackendIdentity();
      eventThread = std::jthread{[this](std::stop_token stopToken) { runEventLoop(stopToken); }};
    }

    ~Impl()
    {
      retireSessions();
      stopEventLoop();

      if (backendPtr)
      {
        backendPtr->stop();
        backendPtr->close();
        resetRenderSession();
      }

      timeline.clear();

      // The render thread is now stopped. Drain the signal ring once more to free
      // any splice signal it posted after stopEventLoop's drain but before the
      // backend stopped, then free any armed-but-never-spliced lookahead node.
      drainRtSignalRing();
      clearPreparedNext();
    }

    Impl(Impl const&) = delete;
    Impl& operator=(Impl const&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    // ── Timeline publication ─────────────────────────────────────
    void publishCurrentNode(std::unique_ptr<TrackNode> nodePtr) { timeline.publishCurrent(std::move(nodePtr)); }
    std::shared_ptr<ISource> currentSource() const { return timeline.current(); }

    // ── Transition state ──────────────────────────────────────────
    static bool isGaplessCapable(DecodedStreamInfo const& info) noexcept
    {
      return !info.isLossy && (info.codec == AudioCodec::Flac || info.codec == AudioCodec::Alac);
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
    void enqueuePlaybackEvent(PlaybackEvent event)
    {
      if (eventThread.get_stop_token().stop_requested())
      {
        return;
      }

      {
        auto const lock = std::scoped_lock{eventMutex};

        if (eventThread.get_stop_token().stop_requested())
        {
          return;
        }

        eventQueue.push_back(std::move(event));
      }

      eventSignal.release();
    }

    // Wait-free producer for the render thread. Pushes a trivially copyable
    // signal onto the lock-free ring and releases the semaphore; no lock, no
    // allocation, no unbounded work. Returns false only if the ring is full,
    // which cannot happen in practice (the event thread drains on every wake and
    // the render thread posts at most one signal per track).
    bool enqueueRtSignal(RtSignal signal) noexcept
    {
      if (!rtSignalRing.push(signal))
      {
        assert(false && "RT signal ring overflow: event thread is not draining");
        return false;
      }

      eventSignal.release();
      return true;
    }

    // Control-command entry point: acquires controlMutex and settles every
    // pending splice signal still in the ring before the command body runs.
    // Between the render thread's raw-pointer publish and the splice signal
    // application, currentSource(), the status fields, and the transition-format
    // snapshot still describe the retired track; settling first closes that
    // window, so a command like seek can never act on the wrong source. Pending
    // drain completions are forwarded to the normal event queue instead: a
    // control command may retire or reposition the render session, and the drain
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
      auto signal = RtSignal{};

      while (rtSignalRing.pop(signal))
      {
        if (signal.kind == RtSignalKind::Drained)
        {
          assert(signal.splicedNode == nullptr);
          enqueuePlaybackEvent(DrainCompleteEvent{.generation = signal.generation, .drainEpoch = signal.drainEpoch});
          continue;
        }

        if (auto notifications = applyRtSignal(signal); !notifications.empty())
        {
          enqueuePlaybackEvent(DeferredNotifications{.notifications = std::move(notifications)});
        }
      }
    }

    void stopEventLoop() noexcept
    {
      if (eventThread.joinable())
      {
        eventThread.request_stop();
        eventSignal.release();
        eventThread.join();
      }

      {
        auto const lock = std::scoped_lock{eventMutex};
        eventQueue.clear();
      }

      drainRtSignalRing();
    }

    // Free any sessions still owned by unprocessed splice signals. Consumer side
    // only (event thread, or the destructor after the event thread has joined).
    void drainRtSignalRing() noexcept
    {
      auto signal = RtSignal{};

      while (rtSignalRing.pop(signal))
      {
        discardSpliceSignalNode(signal.splicedNode);
      }
    }

    static void runNotifications(Notifications& notifications)
    {
      for (auto& notification : notifications)
      {
        if (notification)
        {
          notification();
        }
      }
    }

    void runEventLoop(std::stop_token stopToken)
    {
      while (true)
      {
        eventSignal.acquire();

        if (stopToken.stop_requested())
        {
          return;
        }

        // Drain the wait-free render-thread ring first so a gapless advance is
        // published before any queued non-RT events. The pop itself happens
        // under controlMutex: control threads also consume this ring at command
        // entry (lockControl), settling splice signals and forwarding drain
        // signals, so a control command can never observe a
        // spliced-but-unapplied state.
        while (true)
        {
          auto notifications = Notifications{};
          {
            auto const lock = std::scoped_lock{controlMutex};
            auto signal = RtSignal{};

            if (!rtSignalRing.pop(signal))
            {
              break;
            }

            notifications = applyRtSignal(signal);
          }
          runNotifications(notifications);
        }

        while (true)
        {
          auto optEvent = std::optional<PlaybackEvent>{};
          {
            auto const lock = std::scoped_lock{eventMutex};

            if (eventQueue.empty())
            {
              break;
            }

            optEvent = std::move(eventQueue.front());
            eventQueue.pop_front();
          }

          auto notifications = Notifications{};
          {
            auto const lock = std::scoped_lock{controlMutex};
            notifications = applyPlaybackEvent(*optEvent);
          }
          runNotifications(notifications);
        }
      }
    }

    // Dispatch a render-thread signal. Spliced signals carry a non-owning
    // TrackNode pointer; timeline ownership is already stable. Runs on the
    // event thread under controlMutex.
    Notifications applyRtSignal(RtSignal const& signal)
    {
      switch (signal.kind)
      {
        case RtSignalKind::Spliced: return handleSpliceAdvanced(takeSplicedNode(signal.splicedNode), signal.generation);
        case RtSignalKind::Drained: return handleDrainComplete(signal.generation, signal.drainEpoch);
      }

      return {};
    }

    struct RenderSession final : public IRenderTarget
    {
      RenderSession(Impl& ownerArg, IBackend& backendArg, std::uint64_t generationArg) noexcept
        : owner{ownerArg}, backend{backendArg}, generation{generationArg}
      {
      }

      RenderPcmResult renderPcm(std::span<std::byte> output) noexcept override
      {
        return owner.renderPcm(generation, output);
      }

      void onUnderrun() noexcept override { owner.onUnderrun(generation); }
      void onPositionAdvanced(std::uint32_t frames) noexcept override { owner.onPositionAdvanced(generation, frames); }
      void onDrainComplete() noexcept override { owner.onDrainComplete(generation); }
      void onRouteReady(std::string_view routeAnchor) noexcept override
      {
        owner.onRouteReady(generation, backend, routeAnchor);
      }
      void onFormatChanged(Format const& format) noexcept override { owner.onFormatChanged(generation, format); }
      void onPropertyChanged(PropertyId id) noexcept override { owner.onPropertyChanged(generation, backend, id); }
      void onBackendError(std::string_view message) noexcept override { owner.onBackendError(generation, message); }

      Impl& owner;
      IBackend& backend;
      std::uint64_t generation = 0;
    };

    bool isActiveRenderSession(std::uint64_t generation) const noexcept
    {
      return activeRenderGeneration.load(std::memory_order_acquire) == generation;
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
        [this](std::uint64_t signalGeneration) noexcept { return isActiveRenderSession(signalGeneration); },
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

    void retireRenderSession() noexcept { activeRenderGeneration.store(0, std::memory_order_release); }

    void retireSessions() noexcept
    {
      activeRenderGeneration.store(0, std::memory_order_release);
      timeline.retireCursor();
    }

    void resetRenderSession() noexcept { renderSessionPtr.reset(); }

    IRenderTarget* createRenderSession(IBackend& backend)
    {
      auto const generation = nextRenderGeneration++;
      renderSessionPtr = std::make_unique<RenderSession>(*this, backend, generation);
      activeRenderGeneration.store(generation, std::memory_order_release);
      return renderSessionPtr.get();
    }

    // ── IRenderTarget session entrypoints ─────────────────────────
    RenderPcmResult renderPcm(std::uint64_t generation, std::span<std::byte> output) noexcept
    {
      return detail::renderPcm(
        timeline,
        engineFrameBytes,
        playbackDrainPending,
        generation,
        output,
        [this](std::uint64_t signalGeneration) noexcept { return isActiveRenderSession(signalGeneration); },
        [this](std::uint64_t signalGeneration) noexcept { return trySplicePreparedNext(signalGeneration); });
    }

    void onUnderrun(std::uint64_t generation) noexcept
    {
      if (isActiveRenderSession(generation))
      {
        ++underrunCount;
      }
    }

    void onPositionAdvanced(std::uint64_t generation, std::uint32_t frames) noexcept
    {
      if (isActiveRenderSession(generation))
      {
        accumulatedFrames.fetch_add(frames, std::memory_order_relaxed);
      }
    }

    void onDrainComplete(std::uint64_t generation) noexcept
    {
      if (!isActiveRenderSession(generation))
      {
        return;
      }

      auto const signalDrainEpoch = drainEpoch.load(std::memory_order_acquire);

      if (!playbackDrainPending.exchange(false, std::memory_order_acq_rel))
      {
        return;
      }

      std::ignore = enqueueRtSignal(
        RtSignal{.kind = RtSignalKind::Drained, .generation = generation, .drainEpoch = signalDrainEpoch});
    }

    void onRouteReady(std::uint64_t generation, IBackend& backend, std::string_view routeAnchor) noexcept
    {
      if (!isActiveRenderSession(generation))
      {
        return;
      }

      enqueuePlaybackEvent(RouteReadyEvent{
        .generation = generation, .backendId = backend.backendId(), .routeAnchor = std::string{routeAnchor}});
    }

    void onFormatChanged(std::uint64_t generation, Format const& format) noexcept
    {
      if (isActiveRenderSession(generation))
      {
        enqueuePlaybackEvent(FormatChangedEvent{.generation = generation, .format = format});
      }
    }

    void onPropertyChanged(std::uint64_t generation, IBackend& backend, PropertyId id) noexcept
    {
      if (isActiveRenderSession(generation))
      {
        auto optValue = std::optional<PropertyValue>{};

        if (auto value = backend.property(id); value)
        {
          optValue = *value;
        }

        enqueuePlaybackEvent(PropertyChangedEvent{.generation = generation,
                                                  .id = id,
                                                  .optValue = std::move(optValue),
                                                  .volumeInfo = backend.queryProperty(PropertyId::Volume)});
      }
    }

    void onBackendError(std::uint64_t generation, std::string_view message) noexcept
    {
      if (!isActiveRenderSession(generation))
      {
        return;
      }

      enqueuePlaybackEvent(BackendErrorEvent{.generation = generation, .message = std::string{message}});
    }

    // ── Handlers ───────────────────────────────────────────────────
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

    // Non-const: DeferredNotifications hands its payload out by move.
    Notifications applyPlaybackEvent(PlaybackEvent& event)
    {
      return std::visit([this](auto& typedEvent) { return applyPlaybackEvent(typedEvent); }, event);
    }

    Notifications applyPlaybackEvent(BackendErrorEvent const& event)
    {
      return handleBackendError(event.generation, event.message);
    }

    Notifications applyPlaybackEvent(SourceErrorEvent const& event)
    {
      return handleSourceError(event.sourceGeneration, event.error);
    }

    Notifications applyPlaybackEvent(DrainCompleteEvent const& event)
    {
      return handleDrainComplete(event.generation, event.drainEpoch);
    }

    Notifications applyPlaybackEvent(RouteReadyEvent const& event)
    {
      return handleRouteReady(event.generation, event.backendId, event.routeAnchor);
    }

    Notifications applyPlaybackEvent(FormatChangedEvent const& event)
    {
      return handleFormatChanged(event.generation, event.format);
    }

    Notifications applyPlaybackEvent(PropertyChangedEvent const& event) { return handlePropertyChanged(event); }

    Notifications applyPlaybackEvent(DeferredNotifications& event) { return std::move(event.notifications); }

    static void appendStateChangedNotification(Notifications& notifications, std::function<void()> callback)
    {
      if (callback)
      {
        notifications.emplace_back([callback = std::move(callback)] { callback(); });
      }
    }

    Notifications handleBackendError(std::uint64_t generation, std::string_view message)
    {
      auto stateChanged = std::function<void()>{};
      auto failureCallback = OnPlaybackFailure{};
      auto optFailedItem = std::optional<PlaybackItem>{};
      bool shouldQuiesce = false;
      {
        auto const lock = std::scoped_lock{stateMutex};

        if (!isActiveRenderSession(generation))
        {
          return {};
        }

        optFailedItem = optCurrentItem;
        retireRenderSession();
        resetPlaybackStatePreservingOutput();
        status.transport = Transport::Error;
        status.statusText = std::string{message};
        failureCallback = onPlaybackFailure;
        stateChanged = onStateChanged;
        shouldQuiesce = true;
      }

      resetTransitionState();

      if (shouldQuiesce)
      {
        backendPtr->stop();
        backendPtr->close();
        resetRenderSession();
        timeline.clear();
      }

      auto notifications = Notifications{};

      if (optFailedItem)
      {
        appendPlaybackFailureNotification(
          notifications,
          std::move(failureCallback),
          PlaybackFailure{
            .kind = PlaybackFailureKind::DeviceLost,
            .itemId = optFailedItem->id,
            .input = optFailedItem->input,
            .generation = generation,
            .error = Error{.code = Error::Code::IoError, .message = std::string{message}},
            .recoverable = false,
          });
      }

      appendStateChangedNotification(notifications, std::move(stateChanged));
      return notifications;
    }

    Notifications handleSourceError(std::uint64_t sourceGeneration, Error const& error)
    {
      if (clearPreparedNextForGeneration(sourceGeneration))
      {
        return {};
      }

      auto const message = error.message.empty() ? std::string{"PCM source failed"} : error.message;
      auto endedCallback = std::function<void()>{};
      auto failureCallback = OnPlaybackFailure{};
      auto optFailedItem = std::optional<PlaybackItem>{};
      auto stateChanged = std::function<void()>{};

      bool shouldQuiesce = false;
      {
        auto const lock = std::scoped_lock{stateMutex};

        if (sourceGeneration != timeline.activeSourceGeneration())
        {
          return {};
        }

        if (status.transport == Transport::Idle)
        {
          return {};
        }

        optFailedItem = optCurrentItem;
        retireRenderSession();
        resetPlaybackStatePreservingOutput();
        status.transport = Transport::Error;
        status.statusText = message;
        endedCallback = onTrackEnded;
        failureCallback = onPlaybackFailure;
        stateChanged = onStateChanged;
        shouldQuiesce = true;
      }

      resetTransitionState();

      if (shouldQuiesce)
      {
        backendPtr->stop();
        backendPtr->close();
        resetRenderSession();
        timeline.clear();
      }

      auto notifications = Notifications{};

      if (optFailedItem)
      {
        auto failureError = error;

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
                                            .generation = sourceGeneration,
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

    static void appendRouteNotification(Notifications& notifications, OnRouteChanged callback, RouteStatus snapshot)
    {
      if (callback)
      {
        notifications.emplace_back([callback = std::move(callback), snapshot = std::move(snapshot)]
                                   { callback(snapshot); });
      }
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

    // Complete a gapless splice on the event thread. The render thread already
    // made `session`'s source active; here we install the owning shared_ptr,
    // retire the previous source (destroying it — and joining its decode thread —
    // off the RT thread), refresh the current-track format the next arm gates
    // against, and surface the advance to observers. If the render session was
    // retired between the splice and now, the session is simply dropped (its
    // source is destroyed here, still off the RT thread).
    Notifications handleSpliceAdvanced(TrackNode* session, std::uint64_t generation)
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

        if (!isActiveRenderSession(generation))
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

    Notifications handleRouteReady(std::uint64_t generation, BackendId const& backendId, std::string_view routeAnchor)
    {
      auto notifications = Notifications{};
      auto stateChanged = std::function<void()>{};
      auto callback = OnRouteChanged{};
      auto snapshot = RouteStatus{};
      {
        auto const lock = std::scoped_lock{stateMutex};

        if (!isActiveRenderSession(generation))
        {
          return {};
        }

        routeTracker.setAnchor(backendId, std::string{routeAnchor});
        stateChanged = onStateChanged;
        callback = onRouteChanged;
        snapshot = RouteStatus{.state = routeTracker.state(), .optAnchor = routeTracker.anchor()};
      }

      appendStateChangedNotification(notifications, std::move(stateChanged));
      appendRouteNotification(notifications, std::move(callback), std::move(snapshot));
      return notifications;
    }

    Notifications handleFormatChanged(std::uint64_t generation, Format const& format)
    {
      auto notifications = Notifications{};
      auto stateChanged = std::function<void()>{};
      auto callback = OnRouteChanged{};
      auto snapshot = RouteStatus{};
      {
        auto const lock = std::scoped_lock{stateMutex};

        if (!isActiveRenderSession(generation))
        {
          return {};
        }

        routeTracker.setEngineFormat(format);
        status.routeState.engineOutputFormat = format;
        stateChanged = onStateChanged;
        callback = onRouteChanged;
        snapshot = RouteStatus{.state = routeTracker.state(), .optAnchor = routeTracker.anchor()};
      }

      appendStateChangedNotification(notifications, std::move(stateChanged));
      appendRouteNotification(notifications, std::move(callback), std::move(snapshot));
      return notifications;
    }

    Notifications handlePropertyChanged(PropertyChangedEvent const& event)
    {
      auto notifications = Notifications{};
      auto stateChanged = std::function<void()>{};
      auto callback = OnRouteChanged{};
      auto snapshot = RouteStatus{};
      {
        auto const lock = std::scoped_lock{stateMutex};

        if (!isActiveRenderSession(event.generation))
        {
          return {};
        }

        if (event.id == PropertyId::Volume)
        {
          if (event.optValue)
          {
            if (auto const* vol = std::get_if<float>(&*event.optValue); vol != nullptr)
            {
              status.volume = *vol;
            }
          }
        }
        else if (event.id == PropertyId::Muted)
        {
          if (event.optValue)
          {
            if (auto const* mute = std::get_if<bool>(&*event.optValue); mute != nullptr)
            {
              status.muted = *mute;
            }
          }
        }

        status.volumeAvailable = event.volumeInfo.isAvailable;
        status.volumeIsHardwareAssisted = event.volumeInfo.isHardwareAssisted;
        stateChanged = onStateChanged;
        callback = onRouteChanged;
        snapshot = RouteStatus{.state = routeTracker.state(), .optAnchor = routeTracker.anchor()};
      }

      appendStateChangedNotification(notifications, std::move(stateChanged));
      appendRouteNotification(notifications, std::move(callback), std::move(snapshot));
      return notifications;
    }

    Notifications handleDrainComplete(std::uint64_t generation, std::uint64_t signalDrainEpoch)
    {
      auto onTrackEndedCallback = std::function<void()>{};
      auto onRouteChangedCallback = OnRouteChanged{};
      auto stateChanged = std::function<void()>{};
      bool shouldQuiesce = false;

      {
        auto const lock = std::scoped_lock{stateMutex};

        if (!isActiveRenderSession(generation) || drainEpoch.load(std::memory_order_acquire) != signalDrainEpoch)
        {
          return {};
        }

        retireRenderSession();
        onRouteChangedCallback = onRouteChanged;
        resetPlaybackStatePreservingOutput();
        onTrackEndedCallback = onTrackEnded;
        stateChanged = onStateChanged;
        shouldQuiesce = true;
      }

      resetTransitionState();

      if (shouldQuiesce)
      {
        backendPtr->stop();
        backendPtr->close();
        resetRenderSession();
        timeline.clear();
      }

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

    void setBackendUnlocked(std::unique_ptr<IBackend> nextBackendPtr, Device const& device);
    void updateDeviceUnlocked(Device const& device);
    void playUnlocked(PlaybackItem const& item);
    Result<PreparedNextResult> setNextUnlocked(PlaybackItem const& item);
    std::optional<PlaybackItemId> clearNextUnlocked();
    void pauseUnlocked();
    void resumeUnlocked();
    void stopUnlocked();
    void seekUnlocked(std::chrono::milliseconds offset);
    Result<> setVolumeUnlocked(float volume);
    Result<> setMutedUnlocked(bool muted);
  };

  Engine::Impl::ControlLock::~ControlLock() = default;

  void Engine::Impl::setBackendUnlocked(std::unique_ptr<IBackend> nextBackendPtr, Device const& device)
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
      playUnlocked(*state.optItem);
      seekUnlocked(state.elapsed);

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

  void Engine::Impl::playUnlocked(PlaybackItem const& item)
  {
    resetTransitionState();

    {
      auto const lock = std::scoped_lock{stateMutex};
      retireRenderSession();
      timeline.retireCursor();
    }

    backendPtr->stop();
    backendPtr->close();
    resetRenderSession();
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
    publishCurrentNode(std::move(openedNodePtr));
    auto* renderTarget = createRenderSession(*backendPtr);

    if (auto const openResult = backendPtr->open(currentNode->backendFormat, renderTarget); !openResult)
    {
      retireRenderSession();
      backendPtr->close();
      resetRenderSession();
      auto notifications = Notifications{};
      {
        auto const lock = std::scoped_lock{stateMutex};
        optCurrentItem.reset();
        timeline.retireCursor();
        status.transport = Transport::Error;
        status.statusText = openResult.error().message;
        appendPlaybackFailureNotification(notifications,
                                          onPlaybackFailure,
                                          PlaybackFailure{
                                            .kind = PlaybackFailureKind::RouteActivation,
                                            .itemId = item.id,
                                            .input = item.input,
                                            .generation = sourceGeneration,
                                            .error = openResult.error(),
                                            .recoverable = false,
                                          });
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
      retireRenderSession();
      backendPtr->stop();
      backendPtr->close();
      resetRenderSession();
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
    auto const srcPtr = currentSource();
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

    if (auto const drained = !srcPtr || srcPtr->isDrained();
        drained && (srcPtr ? srcPtr->bufferedDuration() : std::chrono::milliseconds{0}) == std::chrono::milliseconds{0})
    {
      retireRenderSession();
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
      retireRenderSession();
      timeline.retireCursor();
    }

    backendPtr->stop();
    backendPtr->close();
    resetRenderSession();

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
      // (about to be closed) session must not be applied later and resurrect
      // or re-quiesce engine state the reset below already settled.
      retireRenderSession();
      backendPtr->stop();
      backendPtr->close();
      resetRenderSession();
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

  Engine::Engine(std::unique_ptr<IBackend> backendPtr, Device const& device, DecoderFactoryFn decoderFactory)
    : _implPtr{std::make_unique<Impl>(std::move(backendPtr), device, std::move(decoderFactory))}
  {
    _implPtr->syncBackendStatus();
  }

  Engine::~Engine() = default;

  void Engine::setBackend(std::unique_ptr<IBackend> backendPtr, Device const& device)
  {
    auto const controlLock = _implPtr->lockControl();
    _implPtr->setBackendUnlocked(std::move(backendPtr), device);
  }

  void Engine::updateDevice(Device const& device)
  {
    auto const controlLock = _implPtr->lockControl();
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

  void Engine::play(PlaybackItem const& item)
  {
    auto const controlLock = _implPtr->lockControl();
    _implPtr->playUnlocked(item);
  }

  Result<Engine::PreparedNextResult> Engine::setNext(PlaybackItem const& item)
  {
    auto const controlLock = _implPtr->lockControl();
    return _implPtr->setNextUnlocked(item);
  }

  std::optional<Engine::PlaybackItemId> Engine::clearNext()
  {
    auto const controlLock = _implPtr->lockControl();
    return _implPtr->clearNextUnlocked();
  }

  void Engine::pause()
  {
    auto const controlLock = _implPtr->lockControl();
    _implPtr->pauseUnlocked();
  }

  void Engine::resume()
  {
    auto const controlLock = _implPtr->lockControl();
    _implPtr->resumeUnlocked();
  }

  void Engine::stop()
  {
    auto const controlLock = _implPtr->lockControl();
    _implPtr->stopUnlocked();
  }

  void Engine::seek(std::chrono::milliseconds offset)
  {
    auto const controlLock = _implPtr->lockControl();
    _implPtr->seekUnlocked(offset);
  }

  Result<> Engine::setVolume(float volume)
  {
    auto const controlLock = _implPtr->lockControl();
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
