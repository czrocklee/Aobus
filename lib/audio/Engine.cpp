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
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace ao::audio
{
  namespace
  {
  } // namespace

  // ── Engine::Impl: data bucket + callbacks + handlers ────────────

  struct Engine::Impl final
  {
    struct RenderSession;

    Device currentDevice;

    // RT-visible raw pointer to the active source. The render thread
    // (readPcm / isSourceDrained) does a plain lock-free acquire load:
    // std::atomic<shared_ptr> is not lock-free on libstdc++ and must never sit
    // on the audio callback path.
    std::atomic<ISource*> activeSource{nullptr};

    // Owns the active source's lifetime. Touched only off the RT path (play /
    // stop / seek / status) under sourceMutex. The retired source is destroyed
    // only after backendPtr->stop() has quiesced the render thread, so the RT
    // thread can never dereference a freed pointer.
    mutable std::mutex sourceMutex;
    std::shared_ptr<ISource> ownedSourcePtr;

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
    std::atomic<std::uint64_t> activeSourceGeneration{0};
    std::uint64_t nextSourceGeneration = 1;
    std::optional<TrackPlaybackDescriptor> optCurrentTrack;
    Engine::Status status;
    std::function<void()> onTrackEnded;
    Engine::OnRouteChanged onRouteChanged;
    detail::RouteTracker routeTracker;
    DecoderFactoryFn decoderFactory;
    std::unique_ptr<RenderSession> renderSessionPtr;

    // Must be declared last so the PipeWire thread loop is stopped
    // before the callbacks and state it accesses are destroyed.
    std::unique_ptr<IBackend> backendPtr;

    // ── Construction & Destruction ────────────────────────────────
    Impl(std::unique_ptr<IBackend> backendPtr, Device device, DecoderFactoryFn decoderFactory)
      : currentDevice{std::move(device)}, decoderFactory{std::move(decoderFactory)}, backendPtr{std::move(backendPtr)}
    {
      syncBackendIdentity();
    }

    ~Impl()
    {
      retireSessions();

      if (backendPtr)
      {
        backendPtr->stop();
        backendPtr->close();
      }

      publishSource(nullptr);
    }

    Impl(Impl const&) = delete;
    Impl& operator=(Impl const&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    // ── Source publication ────────────────────────────────────────
    // Publish `next` as the active source for the render thread and retire the
    // previous owner. MUST be called only with the backend quiesced (after
    // backendPtr->stop()/close(), or before the first start()), so destroying
    // the retired source — which may join its decode thread — cannot race the
    // RT render thread.
    void publishSource(std::shared_ptr<ISource> nextPtr)
    {
      auto retiredPtr = std::shared_ptr<ISource>{};
      {
        auto const lock = std::scoped_lock{sourceMutex};
        activeSource.store(nextPtr.get(), std::memory_order_release);
        retiredPtr = std::move(ownedSourcePtr);
        ownedSourcePtr = std::move(nextPtr);
      }
      // `retired` is destroyed here, outside the lock.
    }

    void deactivateSourceIfRetired()
    {
      auto const lock = std::scoped_lock{sourceMutex};

      if (activeSourceGeneration.load(std::memory_order_acquire) == 0)
      {
        activeSource.store(nullptr, std::memory_order_release);
      }
    }

    // Copy the owning source pointer for non-RT callers (status / seek / resume),
    // keeping it alive for the duration of their use.
    std::shared_ptr<ISource> currentSource() const
    {
      auto const lock = std::scoped_lock{sourceMutex};

      if (activeSourceGeneration.load(std::memory_order_acquire) == 0)
      {
        return {};
      }

      return ownedSourcePtr;
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

    void resetEngine()
    {
      optCurrentTrack.reset();
      activeSourceGeneration.store(0, std::memory_order_release);
      backendStarted = false;
      playbackDrainPending = false;
      status = {};
      syncBackendIdentity();
      syncBackendStatus();
      accumulatedFrames.store(0, std::memory_order_relaxed);
      routeTracker.clear();
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
      activeSourceGeneration.store(0, std::memory_order_release);
    }

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

      auto* const src = activeSource.load(std::memory_order_acquire);
      return src != nullptr ? src->read(output) : 0;
    }

    bool isSourceDrained(std::uint64_t generation) noexcept
    {
      if (!isActiveRenderSession(generation))
      {
        return true;
      }

      auto* const src = activeSource.load(std::memory_order_acquire);

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

      handleDrainComplete(generation);
    }

    void onRouteReady(std::uint64_t generation, IBackend& backend, std::string_view routeAnchor) noexcept
    {
      if (!isActiveRenderSession(generation))
      {
        return;
      }

      auto anchor = std::string{routeAnchor};
      handleRouteReady(generation, backend, anchor);
    }

    void onFormatChanged(std::uint64_t generation, Format const& format) noexcept
    {
      if (isActiveRenderSession(generation))
      {
        handleFormatChanged(generation, format);
      }
    }

    void onPropertyChanged(std::uint64_t generation, IBackend& backend, PropertyId id) noexcept
    {
      if (isActiveRenderSession(generation))
      {
        handlePropertyChanged(generation, backend, id);
      }
    }

    void onBackendError(std::uint64_t generation, std::string_view message) noexcept
    {
      if (!isActiveRenderSession(generation))
      {
        return;
      }

      auto msg = std::string{message};
      handleBackendError(generation, msg);
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
      activeSourceGeneration.store(0, std::memory_order_release);
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

    void handleBackendError(std::uint64_t generation, std::string_view message)
    {
      AUDIO_LOG_ERROR("Backend error: {}", message);
      {
        auto const lock = std::scoped_lock{stateMutex};

        if (!isActiveRenderSession(generation))
        {
          return;
        }

        retireRenderSession();
        resetPlaybackStatePreservingOutput();
        status.transport = Transport::Error;
        status.statusText = std::string{message};
      }

      deactivateSourceIfRetired();
    }

    void handleSourceError(std::uint64_t sourceGeneration, Error const& error)
    {
      auto const message = error.message.empty() ? std::string{"PCM source failed"} : error.message;
      AUDIO_LOG_ERROR("Source error: {}", message);

      auto const endedCallback = [this]
      {
        auto const lock = std::scoped_lock{stateMutex};
        return onTrackEnded;
      }();

      {
        auto const lock = std::scoped_lock{stateMutex};

        if (sourceGeneration != activeSourceGeneration.load(std::memory_order_acquire))
        {
          return;
        }

        if (status.transport == Transport::Idle)
        {
          return;
        }

        retireRenderSession();
        resetPlaybackStatePreservingOutput();
        status.transport = Transport::Error;
        status.statusText = message;
      }

      deactivateSourceIfRetired();

      if (endedCallback)
      {
        endedCallback();
      }
    }

    void notifyRouteChanged()
    {
      auto callback = Engine::OnRouteChanged{};
      auto snapshot = Engine::RouteStatus{};
      {
        auto const lock = std::scoped_lock{stateMutex};
        callback = onRouteChanged;
        snapshot = Engine::RouteStatus{.state = routeTracker.state(), .optAnchor = routeTracker.anchor()};
      }

      if (callback)
      {
        callback(snapshot);
      }
    }

    void handleRouteReady(std::uint64_t generation, IBackend& backend, std::string_view routeAnchor)
    {
      {
        auto const lock = std::scoped_lock{stateMutex};

        if (!isActiveRenderSession(generation))
        {
          return;
        }

        routeTracker.setAnchor(backend.backendId(), std::string{routeAnchor});
      }

      notifyRouteChanged();
    }

    void handleFormatChanged(std::uint64_t generation, Format const& format)
    {
      {
        auto const lock = std::scoped_lock{stateMutex};

        if (!isActiveRenderSession(generation))
        {
          return;
        }

        routeTracker.setEngineFormat(format);
        status.routeState.engineOutputFormat = format;
      }

      notifyRouteChanged();
    }

    void handlePropertyChanged(std::uint64_t generation, IBackend& backend, PropertyId id)
    {
      {
        auto const lock = std::scoped_lock{stateMutex};

        if (!isActiveRenderSession(generation))
        {
          return;
        }

        if (id == PropertyId::Volume)
        {
          if (auto const vol = backend.get(props::kVolume); vol)
          {
            status.volume = *vol;
          }
        }
        else if (id == PropertyId::Muted)
        {
          if (auto const mute = backend.get(props::kMuted); mute)
          {
            status.muted = *mute;
          }
        }

        auto const volProp = backend.queryProperty(PropertyId::Volume);
        status.volumeAvailable = volProp.isAvailable;
        status.volumeIsHardwareAssisted = volProp.isHardwareAssisted;
      }

      notifyRouteChanged();
    }

    void handleDrainComplete(std::uint64_t generation)
    {
      auto onTrackEndedCallback = std::function<void()>{};
      auto onRouteChangedCallback = Engine::OnRouteChanged{};

      {
        auto const lock = std::scoped_lock{stateMutex};

        if (!isActiveRenderSession(generation))
        {
          return;
        }

        retireRenderSession();
        onRouteChangedCallback = onRouteChanged;
        resetPlaybackStatePreservingOutput();
        onTrackEndedCallback = onTrackEnded;
      }

      deactivateSourceIfRetired();

      if (onRouteChangedCallback)
      {
        onRouteChangedCallback({});
      }

      if (onTrackEndedCallback)
      {
        onTrackEndedCallback();
      }
    }

    // ── Track opening ──────────────────────────────────────────────
    bool openTrack(TrackPlaybackDescriptor const& descriptor,
                   std::shared_ptr<ISource>& source,
                   Format& backendFormat,
                   std::uint64_t sourceGeneration)
    {
      auto session = detail::TrackSession::create(descriptor,
                                                  currentDevice,
                                                  backendPtr->backendId(),
                                                  backendPtr->profileId(),
                                                  decoderFactory,
                                                  [this, sourceGeneration](Error const& err)
                                                  { handleSourceError(sourceGeneration, err); });

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
    void playUnlocked(TrackPlaybackDescriptor const& descriptor);
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
      std::optional<TrackPlaybackDescriptor> optTrack;
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
      AUDIO_LOG_INFO("Resuming track '{}' after backend switch", state.optTrack->title);
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

  void Engine::Impl::playUnlocked(TrackPlaybackDescriptor const& descriptor)
  {
    AUDIO_LOG_INFO("Play requested: {} - {} [{}]", descriptor.artist, descriptor.title, descriptor.filePath.string());

    {
      auto const lock = std::scoped_lock{stateMutex};
      retireRenderSession();
      activeSourceGeneration.store(0, std::memory_order_release);
    }

    backendPtr->stop();
    backendPtr->close();
    publishSource(nullptr);

    auto sourcePtr = std::shared_ptr<ISource>{};
    auto backendFormat = Format{};
    auto const sourceGeneration = nextSourceGeneration++;

    {
      auto const lock = std::scoped_lock{stateMutex};
      underrunCount = 0;
      routeTracker.clear();
      activeSourceGeneration.store(sourceGeneration, std::memory_order_release);
      backendStarted = false;
      playbackDrainPending = false;
      status.transport = Transport::Opening;
      optCurrentTrack = descriptor;
      syncBackendIdentity();
    }

    if (!openTrack(descriptor, sourcePtr, backendFormat, sourceGeneration))
    {
      auto const lock = std::scoped_lock{stateMutex};
      AUDIO_LOG_ERROR("Failed to open track '{}': {}", descriptor.filePath.string(), status.statusText);
      status.transport = Transport::Error;
      activeSourceGeneration.store(0, std::memory_order_release);
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
      AUDIO_LOG_ERROR("Failed to open backend for '{}': {}", descriptor.filePath.string(), openResult.error().message);
      {
        auto const lock = std::scoped_lock{stateMutex};
        optCurrentTrack.reset();
        activeSourceGeneration.store(0, std::memory_order_release);
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
      publishSource(nullptr);
      auto const lock = std::scoped_lock{stateMutex};
      resetEngine();
      return;
    }

    {
      auto const lock = std::scoped_lock{stateMutex};

      // A source decode error can fire asynchronously (handleSourceError) while
      // this operation is still mid-setup. Error is terminal: never clobber it
      // with Playing. The check and the write share this critical section with
      // handleSourceError's, so Error wins regardless of interleaving and the
      // backend is not started behind a stopped/dead source.
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
      activeSourceGeneration.store(0, std::memory_order_release);
    }

    backendPtr->stop();
    backendPtr->close();

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

      // A source decode error can fire asynchronously (handleSourceError) while
      // this operation is still mid-setup. Error is terminal: never clobber it
      // with Playing. The check and the write share this critical section with
      // handleSourceError's, so Error wins regardless of interleaving and the
      // backend is not started behind a stopped/dead source.
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

  void Engine::play(TrackPlaybackDescriptor const& descriptor)
  {
    auto const controlLock = std::scoped_lock{_implPtr->controlMutex};
    _implPtr->playUnlocked(descriptor);
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
