// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "detail/TrackSession.h"
#include <ao/Error.h>
#include <ao/audio/Backend.h>
#include <ao/audio/Engine.h>
#include <ao/audio/Format.h>
#include <ao/audio/IBackend.h>
#include <ao/audio/IRenderTarget.h>
#include <ao/audio/ISource.h>
#include <ao/audio/Property.h>
#include <ao/audio/Types.h>
#include <ao/audio/detail/RouteTracker.h>
#include <ao/utility/Log.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
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

    struct DrainCompleteEvent final
    {
      std::uint64_t generation = 0;
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

    using PlaybackEvent = std::variant<BackendErrorEvent,
                                       SourceErrorEvent,
                                       DrainCompleteEvent,
                                       RouteReadyEvent,
                                       FormatChangedEvent,
                                       PropertyChangedEvent>;
    using Notification = std::function<void()>;
    using Notifications = std::vector<Notification>;

    Device currentDevice;

    class RenderSourceSlot final
    {
    public:
      ISource* active() const noexcept { return _active.load(std::memory_order_acquire); }

      std::uint64_t generation() const noexcept { return _generation.load(std::memory_order_acquire); }

      void setGeneration(std::uint64_t generation) noexcept
      {
        _generation.store(generation, std::memory_order_release);
      }

      void retireGeneration() noexcept { _generation.store(0, std::memory_order_release); }

      // Publish `next` as the active source for the render thread and retire the
      // previous owner. MUST be called only with the backend quiesced (after
      // backendPtr->stop()/close(), or before the first start()), so destroying
      // the retired source — which may join its decode thread — cannot race the
      // RT render thread.
      void publish(std::shared_ptr<ISource> nextPtr)
      {
        auto retiredPtr = std::shared_ptr<ISource>{};
        {
          auto const lock = std::scoped_lock{_mutex};
          _active.store(nextPtr.get(), std::memory_order_release);
          retiredPtr = std::move(_ownerPtr);
          _ownerPtr = std::move(nextPtr);
        }
        // `retired` is destroyed here, outside the lock.
      }

      // Copy the owning source pointer for non-RT callers (status / seek /
      // resume), keeping it alive for the duration of their use.
      std::shared_ptr<ISource> current() const
      {
        auto const lock = std::scoped_lock{_mutex};

        if (_generation.load(std::memory_order_acquire) == 0)
        {
          return {};
        }

        return _ownerPtr;
      }

    private:
      std::atomic<ISource*> _active{nullptr};
      std::atomic<std::uint64_t> _generation{0};
      mutable std::mutex _mutex;
      std::shared_ptr<ISource> _ownerPtr;
    };

    RenderSourceSlot sourceSlot;

    // Serializes external control commands that coordinate backend lifecycle,
    // source ownership, and status publication. Backend callbacks deliberately
    // do not take this lock; they may run on the render thread while stop()
    // waits for that thread to quiesce.
    mutable std::mutex controlMutex;

    std::atomic<bool> backendStarted{false};
    std::atomic<bool> playbackDrainPending{false};
    std::atomic<std::uint32_t> underrunCount{0};
    std::atomic<std::uint64_t> accumulatedFrames{0};
    std::atomic<std::uint32_t> engineSampleRate{0};
    std::atomic<std::uint64_t> activeRenderGeneration{0};

    mutable std::mutex stateMutex;
    std::uint64_t nextRenderGeneration = 1;
    std::uint64_t nextSourceGeneration = 1;
    std::optional<PlaybackInput> optCurrentTrack;
    Engine::Status status;
    std::function<void()> onTrackEnded;
    std::function<void()> onStateChanged;
    Engine::OnRouteChanged onRouteChanged;
    detail::RouteTracker routeTracker;
    DecoderFactoryFn decoderFactory;
    std::unique_ptr<RenderSession> renderSessionPtr;

    mutable std::mutex eventMutex;
    std::condition_variable_any eventCv;
    std::deque<PlaybackEvent> eventQueue;
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

      publishSource(nullptr);
    }

    Impl(Impl const&) = delete;
    Impl& operator=(Impl const&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    // ── Source publication ────────────────────────────────────────
    void publishSource(std::shared_ptr<ISource> nextPtr) { sourceSlot.publish(std::move(nextPtr)); }
    std::shared_ptr<ISource> currentSource() const { return sourceSlot.current(); }

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

    void resetEngine()
    {
      optCurrentTrack.reset();
      sourceSlot.retireGeneration();
      backendStarted = false;
      playbackDrainPending = false;
      status = {};
      syncBackendIdentity();
      syncBackendStatus();
      accumulatedFrames.store(0, std::memory_order_relaxed);
      routeTracker.clear();
    }

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

      eventCv.notify_one();
    }

    void stopEventLoop() noexcept
    {
      if (eventThread.joinable())
      {
        eventThread.request_stop();
        eventCv.notify_all();
        eventThread.join();
      }

      auto const lock = std::scoped_lock{eventMutex};
      eventQueue.clear();
    }

    void runEventLoop(std::stop_token stopToken)
    {
      while (true)
      {
        auto optEvent = std::optional<PlaybackEvent>{};
        {
          auto lock = std::unique_lock{eventMutex};
          eventCv.wait(lock, stopToken, [this] { return !eventQueue.empty(); });

          if (stopToken.stop_requested())
          {
            eventQueue.clear();
            return;
          }

          optEvent = std::move(eventQueue.front());
          eventQueue.pop_front();
        }

        auto notifications = Notifications{};
        {
          auto const lock = std::scoped_lock{controlMutex};
          notifications = applyPlaybackEvent(*optEvent);
        }

        for (auto& notification : notifications)
        {
          if (notification)
          {
            notification();
          }
        }
      }
    }

    struct RenderSession final : public IRenderTarget
    {
      RenderSession(Impl& ownerArg, IBackend& backendArg, std::uint64_t generationArg) noexcept
        : owner{ownerArg}, backend{backendArg}, generation{generationArg}
      {
      }

      std::size_t readPcm(std::span<std::byte> output) noexcept override { return owner.readPcm(generation, output); }

      bool isSourceDrained() noexcept override { return owner.isSourceDrained(generation); }
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

    void retireRenderSession() noexcept { activeRenderGeneration.store(0, std::memory_order_release); }

    void retireSessions() noexcept
    {
      activeRenderGeneration.store(0, std::memory_order_release);
      sourceSlot.retireGeneration();
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
    std::size_t readPcm(std::uint64_t generation, std::span<std::byte> output) const noexcept
    {
      if (!isActiveRenderSession(generation))
      {
        return 0;
      }

      auto* const src = sourceSlot.active();
      return src != nullptr ? src->read(output) : 0;
    }

    bool isSourceDrained(std::uint64_t generation) noexcept
    {
      if (!isActiveRenderSession(generation))
      {
        return true;
      }

      auto* const src = sourceSlot.active();

      if (src == nullptr)
      {
        return true;
      }

      if (src->isDrained())
      {
        playbackDrainPending = true;
        return true;
      }

      return false;
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

      if (!playbackDrainPending.exchange(false, std::memory_order_relaxed))
      {
        return;
      }

      enqueuePlaybackEvent(DrainCompleteEvent{.generation = generation});
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

      optCurrentTrack.reset();
      sourceSlot.retireGeneration();
      backendStarted = false;
      playbackDrainPending = false;
      status = {};
      status.backendId = backendId;
      status.profileId = profileId;
      status.currentDeviceId = currentDeviceId;
      status.volume = volume;
      status.muted = muted;
      status.volumeAvailable = volumeAvailable;
      status.volumeIsHardwareAssisted = volumeIsHardwareAssisted;
      accumulatedFrames.store(0, std::memory_order_relaxed);
      routeTracker.clear();
    }

    Notifications applyPlaybackEvent(PlaybackEvent const& event)
    {
      return std::visit([this](auto const& typedEvent) { return applyPlaybackEvent(typedEvent); }, event);
    }

    Notifications applyPlaybackEvent(BackendErrorEvent const& event)
    {
      return handleBackendError(event.generation, event.message);
    }

    Notifications applyPlaybackEvent(SourceErrorEvent const& event)
    {
      return handleSourceError(event.sourceGeneration, event.error);
    }

    Notifications applyPlaybackEvent(DrainCompleteEvent const& event) { return handleDrainComplete(event.generation); }

    Notifications applyPlaybackEvent(RouteReadyEvent const& event)
    {
      return handleRouteReady(event.generation, event.backendId, event.routeAnchor);
    }

    Notifications applyPlaybackEvent(FormatChangedEvent const& event)
    {
      return handleFormatChanged(event.generation, event.format);
    }

    Notifications applyPlaybackEvent(PropertyChangedEvent const& event) { return handlePropertyChanged(event); }

    static void appendStateChangedNotification(Notifications& notifications, std::function<void()> callback)
    {
      if (callback)
      {
        notifications.emplace_back([callback = std::move(callback)] { callback(); });
      }
    }

    Notifications handleBackendError(std::uint64_t generation, std::string_view message)
    {
      AUDIO_LOG_ERROR("Backend error: {}", message);
      auto stateChanged = std::function<void()>{};
      bool shouldQuiesce = false;
      {
        auto const lock = std::scoped_lock{stateMutex};

        if (!isActiveRenderSession(generation))
        {
          return {};
        }

        retireRenderSession();
        resetPlaybackStatePreservingOutput();
        status.transport = Transport::Error;
        status.statusText = std::string{message};
        stateChanged = onStateChanged;
        shouldQuiesce = true;
      }

      if (shouldQuiesce)
      {
        backendPtr->stop();
        backendPtr->close();
        resetRenderSession();
        publishSource(nullptr);
      }

      auto notifications = Notifications{};
      appendStateChangedNotification(notifications, std::move(stateChanged));
      return notifications;
    }

    Notifications handleSourceError(std::uint64_t sourceGeneration, Error const& error)
    {
      auto const message = error.message.empty() ? std::string{"PCM source failed"} : error.message;
      AUDIO_LOG_ERROR("Source error: {}", message);
      auto endedCallback = std::function<void()>{};
      auto stateChanged = std::function<void()>{};

      bool shouldQuiesce = false;
      {
        auto const lock = std::scoped_lock{stateMutex};

        if (sourceGeneration != sourceSlot.generation())
        {
          return {};
        }

        if (status.transport == Transport::Idle)
        {
          return {};
        }

        retireRenderSession();
        resetPlaybackStatePreservingOutput();
        status.transport = Transport::Error;
        status.statusText = message;
        endedCallback = onTrackEnded;
        stateChanged = onStateChanged;
        shouldQuiesce = true;
      }

      if (shouldQuiesce)
      {
        backendPtr->stop();
        backendPtr->close();
        resetRenderSession();
        publishSource(nullptr);
      }

      auto notifications = Notifications{};
      appendStateChangedNotification(notifications, std::move(stateChanged));

      if (endedCallback)
      {
        notifications.emplace_back([callback = std::move(endedCallback)] { callback(); });
      }

      return notifications;
    }

    static void appendRouteNotification(Notifications& notifications,
                                        Engine::OnRouteChanged callback,
                                        Engine::RouteStatus snapshot)
    {
      if (callback)
      {
        notifications.emplace_back([callback = std::move(callback), snapshot = std::move(snapshot)]
                                   { callback(snapshot); });
      }
    }

    Notifications handleRouteReady(std::uint64_t generation, BackendId const& backendId, std::string_view routeAnchor)
    {
      auto notifications = Notifications{};
      auto stateChanged = std::function<void()>{};
      auto callback = Engine::OnRouteChanged{};
      auto snapshot = Engine::RouteStatus{};
      {
        auto const lock = std::scoped_lock{stateMutex};

        if (!isActiveRenderSession(generation))
        {
          return {};
        }

        routeTracker.setAnchor(backendId, std::string{routeAnchor});
        stateChanged = onStateChanged;
        callback = onRouteChanged;
        snapshot = Engine::RouteStatus{.state = routeTracker.state(), .optAnchor = routeTracker.anchor()};
      }

      appendStateChangedNotification(notifications, std::move(stateChanged));
      appendRouteNotification(notifications, std::move(callback), std::move(snapshot));
      return notifications;
    }

    Notifications handleFormatChanged(std::uint64_t generation, Format const& format)
    {
      auto notifications = Notifications{};
      auto stateChanged = std::function<void()>{};
      auto callback = Engine::OnRouteChanged{};
      auto snapshot = Engine::RouteStatus{};
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
        snapshot = Engine::RouteStatus{.state = routeTracker.state(), .optAnchor = routeTracker.anchor()};
      }

      appendStateChangedNotification(notifications, std::move(stateChanged));
      appendRouteNotification(notifications, std::move(callback), std::move(snapshot));
      return notifications;
    }

    Notifications handlePropertyChanged(PropertyChangedEvent const& event)
    {
      auto notifications = Notifications{};
      auto stateChanged = std::function<void()>{};
      auto callback = Engine::OnRouteChanged{};
      auto snapshot = Engine::RouteStatus{};
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
        snapshot = Engine::RouteStatus{.state = routeTracker.state(), .optAnchor = routeTracker.anchor()};
      }

      appendStateChangedNotification(notifications, std::move(stateChanged));
      appendRouteNotification(notifications, std::move(callback), std::move(snapshot));
      return notifications;
    }

    Notifications handleDrainComplete(std::uint64_t generation)
    {
      auto onTrackEndedCallback = std::function<void()>{};
      auto onRouteChangedCallback = Engine::OnRouteChanged{};
      auto stateChanged = std::function<void()>{};
      bool shouldQuiesce = false;

      {
        auto const lock = std::scoped_lock{stateMutex};

        if (!isActiveRenderSession(generation))
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

      if (shouldQuiesce)
      {
        backendPtr->stop();
        backendPtr->close();
        resetRenderSession();
        publishSource(nullptr);
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
    bool openTrack(PlaybackInput const& input,
                   std::shared_ptr<ISource>& source,
                   Format& backendFormat,
                   std::uint64_t sourceGeneration)
    {
      auto session = detail::TrackSession::create(
        input,
        currentDevice,
        backendPtr->backendId(),
        backendPtr->profileId(),
        decoderFactory,
        [this, sourceGeneration](Error const& err)
        { enqueuePlaybackEvent(SourceErrorEvent{.sourceGeneration = sourceGeneration, .error = err}); });

      if (!session)
      {
        auto const lock = std::scoped_lock{stateMutex};
        status.statusText = session.error.message;
        return false;
      }

      source = std::move(session.sourcePtr);
      backendFormat = session.backendFormat;

      // TrackSession::create() ran lock-free above; only the status/routeTracker
      // publication needs the lock, which status() also takes when it reads them
      // concurrently from the UI thread.
      auto const lock = std::scoped_lock{stateMutex};
      status.duration = session.info.duration;
      status.elapsed = std::chrono::milliseconds{0};
      accumulatedFrames.store(0, std::memory_order_relaxed);
      routeTracker.setDecoder(
        session.info.sourceFormat, session.info.outputFormat, session.info.isLossy, session.info.codec);
      routeTracker.setEngineFormat(session.info.outputFormat);
      status.routeState = routeTracker.state();
      engineSampleRate.store(session.info.outputFormat.sampleRate, std::memory_order_relaxed);

      return true;
    }

    void setBackendUnlocked(std::unique_ptr<IBackend> nextBackendPtr, Device const& device);
    void updateDeviceUnlocked(Device const& device);
    void playUnlocked(PlaybackInput const& input);
    void pauseUnlocked();
    void resumeUnlocked();
    void stopUnlocked();
    void seekUnlocked(std::chrono::milliseconds offset);
    void setVolumeUnlocked(float volume);
    void setMutedUnlocked(bool muted);
  };

  void Engine::Impl::setBackendUnlocked(std::unique_ptr<IBackend> nextBackendPtr, Device const& device)
  {
    struct State
    {
      std::optional<PlaybackInput> optTrack;
      std::chrono::milliseconds elapsed{0};
      bool wasPlaying = false;
    };

    auto const state = [this]
    {
      auto const lock = std::scoped_lock{stateMutex};
      return State{
        .optTrack = optCurrentTrack,
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

    if (state.optTrack)
    {
      AUDIO_LOG_INFO("Resuming {} after backend switch", state.optTrack->filePath.string());
      playUnlocked(*state.optTrack);
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
  }

  void Engine::Impl::playUnlocked(PlaybackInput const& input)
  {
    AUDIO_LOG_INFO("Play requested: {}", input.filePath.string());

    {
      auto const lock = std::scoped_lock{stateMutex};
      retireRenderSession();
      sourceSlot.retireGeneration();
    }

    backendPtr->stop();
    backendPtr->close();
    resetRenderSession();
    publishSource(nullptr);

    auto sourcePtr = std::shared_ptr<ISource>{};
    auto backendFormat = Format{};
    auto const sourceGeneration = nextSourceGeneration++;

    {
      auto const lock = std::scoped_lock{stateMutex};
      underrunCount = 0;
      routeTracker.clear();
      sourceSlot.setGeneration(sourceGeneration);
      backendStarted = false;
      playbackDrainPending = false;
      status.transport = Transport::Opening;
      optCurrentTrack = input;
      syncBackendIdentity();
    }

    if (!openTrack(input, sourcePtr, backendFormat, sourceGeneration))
    {
      auto const lock = std::scoped_lock{stateMutex};
      AUDIO_LOG_ERROR("Failed to open track '{}': {}", input.filePath.string(), status.statusText);
      status.transport = Transport::Error;
      sourceSlot.retireGeneration();
      optCurrentTrack.reset();
      return;
    }

    {
      auto const lock = std::scoped_lock{stateMutex};
      status.transport = Transport::Buffering;
    }

    publishSource(sourcePtr);
    auto* renderTarget = createRenderSession(*backendPtr);

    if (auto const openResult = backendPtr->open(backendFormat, renderTarget); !openResult)
    {
      retireRenderSession();
      backendPtr->close();
      resetRenderSession();
      AUDIO_LOG_ERROR("Failed to open backend for '{}': {}", input.filePath.string(), openResult.error().message);
      {
        auto const lock = std::scoped_lock{stateMutex};
        optCurrentTrack.reset();
        sourceSlot.retireGeneration();
        status.transport = Transport::Error;
        status.statusText = openResult.error().message;
      }
      publishSource(nullptr);
      return;
    }

    {
      auto const lock = std::scoped_lock{stateMutex};
      syncBackendStatus();
    }

    auto const bufferedDuration = sourcePtr ? sourcePtr->bufferedDuration() : std::chrono::milliseconds{0};

    if (auto const drained = !sourcePtr || sourcePtr->isDrained();
        drained && bufferedDuration == std::chrono::milliseconds{0})
    {
      retireRenderSession();
      backendPtr->stop();
      backendPtr->close();
      resetRenderSession();
      publishSource(nullptr);
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

  void Engine::Impl::pauseUnlocked()
  {
    bool shouldPause = false;
    {
      auto const lock = std::scoped_lock{stateMutex};

      if (status.transport == Transport::Playing || status.transport == Transport::Buffering)
      {
        AUDIO_LOG_INFO("Playback paused");
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

    AUDIO_LOG_INFO("Playback resumed");

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
      publishSource(nullptr);
      return;
    }

    status.transport = Transport::Playing;
    backendStarted = true;
    lock.unlock();
    backendPtr->start();
  }

  void Engine::Impl::stopUnlocked()
  {
    AUDIO_LOG_INFO("Playback stopped");

    {
      auto const lock = std::scoped_lock{stateMutex};
      retireRenderSession();
      sourceSlot.retireGeneration();
    }

    backendPtr->stop();
    backendPtr->close();
    resetRenderSession();

    {
      auto const lock = std::scoped_lock{stateMutex};
      resetEngine();
    }

    publishSource(nullptr);
  }

  void Engine::Impl::seekUnlocked(std::chrono::milliseconds offset)
  {
    AUDIO_LOG_INFO("Seek requested: {} ms", offset.count());
    auto const sourcePtr = currentSource();

    if (!sourcePtr)
    {
      return;
    }

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
    playbackDrainPending = false;

    if (auto const seekResult = sourcePtr->seek(offset); !seekResult)
    {
      AUDIO_LOG_ERROR("Seek failed at {} ms: {}", offset.count(), seekResult.error().message);
      auto const lock = std::scoped_lock{stateMutex};
      status.transport = Transport::Error;
      status.statusText = seekResult.error().message;
      return;
    }

    auto const bufferedDuration = sourcePtr->bufferedDuration();

    if (auto const drained = sourcePtr->isDrained(); drained && bufferedDuration == std::chrono::milliseconds{0})
    {
      backendPtr->stop();
      backendPtr->close();
      resetRenderSession();
      publishSource(nullptr);
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

  void Engine::Impl::setVolumeUnlocked(float volume)
  {
    if (auto const result = backendPtr->set(props::kVolume, volume); !result)
    {
      AUDIO_LOG_ERROR("Failed to set volume: {}", result.error().message);
    }

    auto const lock = std::scoped_lock{stateMutex};
    status.volume = volume;
  }

  void Engine::Impl::setMutedUnlocked(bool muted)
  {
    if (auto const result = backendPtr->set(props::kMuted, muted); !result)
    {
      AUDIO_LOG_ERROR("Failed to set muted state: {}", result.error().message);
    }

    auto const lock = std::scoped_lock{stateMutex};
    status.muted = muted;
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
    auto const controlLock = std::scoped_lock{_implPtr->controlMutex};
    _implPtr->setBackendUnlocked(std::move(backendPtr), device);
  }

  void Engine::updateDevice(Device const& device)
  {
    auto const controlLock = std::scoped_lock{_implPtr->controlMutex};
    _implPtr->updateDeviceUnlocked(device);
  }

  void Engine::setOnTrackEnded(std::function<void()> callback)
  {
    auto const lock = std::scoped_lock{_implPtr->stateMutex};
    _implPtr->onTrackEnded = std::move(callback);
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
    auto snap = Engine::Status{_implPtr->status};
    auto const totalFrames = _implPtr->accumulatedFrames.load(std::memory_order_relaxed);
    auto const sampleRate = _implPtr->engineSampleRate.load(std::memory_order_relaxed);
    snap.elapsed = samplesToDuration(totalFrames, sampleRate);
    snap.routeState = _implPtr->routeTracker.state();
    snap.bufferedDuration = sourcePtr ? sourcePtr->bufferedDuration() : std::chrono::milliseconds{0};
    snap.underrunCount = _implPtr->underrunCount.load(std::memory_order_relaxed);
    return snap;
  }

  void Engine::play(PlaybackInput const& input)
  {
    auto const controlLock = std::scoped_lock{_implPtr->controlMutex};
    _implPtr->playUnlocked(input);
  }

  void Engine::pause()
  {
    auto const controlLock = std::scoped_lock{_implPtr->controlMutex};
    _implPtr->pauseUnlocked();
  }

  void Engine::resume()
  {
    auto const controlLock = std::scoped_lock{_implPtr->controlMutex};
    _implPtr->resumeUnlocked();
  }

  void Engine::stop()
  {
    auto const controlLock = std::scoped_lock{_implPtr->controlMutex};
    _implPtr->stopUnlocked();
  }

  void Engine::seek(std::chrono::milliseconds offset)
  {
    auto const controlLock = std::scoped_lock{_implPtr->controlMutex};
    _implPtr->seekUnlocked(offset);
  }

  void Engine::setVolume(float volume)
  {
    auto const controlLock = std::scoped_lock{_implPtr->controlMutex};
    _implPtr->setVolumeUnlocked(volume);
  }

  float Engine::volume() const
  {
    auto const lock = std::scoped_lock{_implPtr->stateMutex};
    return _implPtr->status.volume;
  }

  void Engine::setMuted(bool muted)
  {
    auto const controlLock = std::scoped_lock{_implPtr->controlMutex};
    _implPtr->setMutedUnlocked(muted);
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
