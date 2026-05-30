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
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

namespace ao::audio
{
  namespace
  {
  } // namespace

  // ── Engine::Impl: data bucket + callbacks + handlers ────────────

  struct Engine::Impl final : public IRenderTarget
  {
    Device currentDevice;

    std::atomic<std::shared_ptr<ISource>> source;
    std::atomic<bool> backendStarted{false};
    std::atomic<bool> playbackDrainPending{false};
    std::atomic<std::uint32_t> underrunCount{0};
    std::atomic<std::uint64_t> accumulatedFrames{0};
    std::atomic<std::uint32_t> engineSampleRate{0};

    mutable std::mutex stateMutex;
    std::optional<TrackPlaybackDescriptor> optCurrentTrack;
    Engine::Status status;
    std::function<void()> onTrackEnded;
    Engine::OnRouteChanged onRouteChanged;
    detail::RouteTracker routeTracker;
    DecoderFactoryFn decoderFactory;

    // Must be declared last so the PipeWire thread loop is stopped
    // before the callbacks and state it accesses are destroyed.
    std::unique_ptr<IBackend> backendPtr;

    // ── Construction & Destruction ────────────────────────────────
    Impl(std::unique_ptr<IBackend> backendPtr, Device device, DecoderFactoryFn decoderFactory)
      : currentDevice{std::move(device)}, decoderFactory{std::move(decoderFactory)}, backendPtr{std::move(backendPtr)}
    {
      syncBackendIdentity();
    }

    ~Impl() override
    {
      // Stop the source and its background thread first. This prevents the
      // decode thread from firing error callbacks (which access `backend`)
      // after `backend` has been destroyed.
      source.store(nullptr, std::memory_order_release);
    }

    Impl(Impl const&) = delete;
    Impl& operator=(Impl const&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

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
      backendStarted = false;
      playbackDrainPending = false;
      status = {};
      syncBackendIdentity();
      syncBackendStatus();
      accumulatedFrames.store(0, std::memory_order_relaxed);
      routeTracker.clear();
    }

    // ── IRenderTarget Overrides ───────────────────────────────────
    std::size_t readPcm(std::span<std::byte> output) noexcept override
    {
      auto const srcPtr = source.load(std::memory_order_acquire);
      return srcPtr ? srcPtr->read(output) : 0;
    }

    bool isSourceDrained() noexcept override
    {
      auto const srcPtr = source.load(std::memory_order_acquire);

      if (!srcPtr)
      {
        return true;
      }

      if (srcPtr->isDrained())
      {
        playbackDrainPending = true;
        return true;
      }

      return false;
    }

    void onUnderrun() noexcept override { ++underrunCount; }

    void onPositionAdvanced(std::uint32_t frames) noexcept override
    {
      accumulatedFrames.fetch_add(frames, std::memory_order_relaxed);
    }

    void onDrainComplete() noexcept override
    {
      if (!playbackDrainPending.exchange(false, std::memory_order_relaxed))
      {
        return;
      }

      handleDrainComplete();
    }

    void onRouteReady(std::string_view routeAnchor) noexcept override
    {
      auto anchor = std::string{routeAnchor};
      handleRouteReady(anchor);
    }

    void onFormatChanged(Format const& format) noexcept override { handleFormatChanged(format); }

    void onPropertyChanged(PropertyId id) noexcept override { handlePropertyChanged(id); }

    void onBackendError(std::string_view message) noexcept override
    {
      auto msg = std::string{message};
      handleBackendError(msg);
    }

    // ── Handlers ───────────────────────────────────────────────────
    void handleBackendError(std::string_view message)
    {
      AUDIO_LOG_ERROR("Backend error: {}", message);
      backendPtr->stop();
      backendPtr->close();
      source.store({}, std::memory_order_release);
      auto const lock = std::scoped_lock{stateMutex};
      resetEngine();
      status.transport = Transport::Error;
      status.statusText = std::string{message};
    }

    void handleSourceError(Error const& error)
    {
      auto const endedCallback = [this]
      {
        auto const lock = std::scoped_lock{stateMutex};
        return onTrackEnded;
      }();

      {
        auto const lock = std::scoped_lock{stateMutex};

        if (status.transport == Transport::Idle)
        {
          return;
        }

        backendStarted = false;
        playbackDrainPending = false;
        status.transport = Transport::Error;
        status.statusText = error.message.empty() ? "PCM source failed" : error.message;
      }

      backendPtr->stop();

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

    void handleRouteReady(std::string_view routeAnchor)
    {
      routeTracker.setAnchor(backendPtr->backendId(), std::string{routeAnchor});
      notifyRouteChanged();
    }

    void handleFormatChanged(Format const& format)
    {
      {
        auto const lock = std::scoped_lock{stateMutex};
        routeTracker.setEngineFormat(format);
        status.routeState.engineOutputFormat = format;
      }

      notifyRouteChanged();
    }

    void handlePropertyChanged(PropertyId id)
    {
      {
        auto const lock = std::scoped_lock{stateMutex};

        if (id == PropertyId::Volume)
        {
          if (auto const vol = backendPtr->get(props::kVolume); vol)
          {
            status.volume = *vol;
          }
        }
        else if (id == PropertyId::Muted)
        {
          if (auto const mute = backendPtr->get(props::kMuted); mute)
          {
            status.muted = *mute;
          }
        }

        auto const volProp = backendPtr->queryProperty(PropertyId::Volume);
        status.volumeAvailable = volProp.isAvailable;
        status.volumeIsHardwareAssisted = volProp.isHardwareAssisted;
      }

      notifyRouteChanged();
    }

    void handleDrainComplete()
    {
      source.store({}, std::memory_order_release);

      auto onTrackEndedCallback = std::function<void()>{};
      auto onRouteChangedCallback = Engine::OnRouteChanged{};

      {
        auto const lock = std::scoped_lock{stateMutex};
        onRouteChangedCallback = onRouteChanged;
        resetEngine();
        onTrackEndedCallback = onTrackEnded;
      }

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
    bool openTrack(TrackPlaybackDescriptor const& descriptor, std::shared_ptr<ISource>& source, Format& backendFormat)
    {
      auto session = detail::TrackSession::create(descriptor,
                                                  currentDevice,
                                                  backendPtr->backendId(),
                                                  backendPtr->profileId(),
                                                  decoderFactory,
                                                  [this](Error const& err) { handleSourceError(err); });

      if (!session)
      {
        status.statusText = session.error.message;
        return false;
      }

      source = std::move(session.sourcePtr);
      backendFormat = session.backendFormat;

      status.durationMs = session.info.durationMs;
      status.positionMs = 0;
      accumulatedFrames.store(0, std::memory_order_relaxed);
      routeTracker.setDecoder(session.info.sourceFormat, session.info.outputFormat, session.info.isLossy);
      routeTracker.setEngineFormat(session.info.outputFormat);
      status.routeState = routeTracker.state();
      engineSampleRate.store(session.info.outputFormat.sampleRate, std::memory_order_relaxed);

      return true;
    }
  };

  // ── Engine ──────────────────────────────────────────────────────

  Engine::Engine(std::unique_ptr<IBackend> backendPtr, Device const& device, DecoderFactoryFn decoderFactory)
    : _implPtr{std::make_unique<Impl>(std::move(backendPtr), device, std::move(decoderFactory))}
  {
    _implPtr->syncBackendStatus();
  }

  Engine::~Engine() = default;

  void Engine::setBackend(std::unique_ptr<IBackend> backendPtr, Device const& device)
  {
    struct State
    {
      std::optional<TrackPlaybackDescriptor> optTrack;
      std::uint32_t positionMs = 0;
      bool wasPlaying = false;
    };

    auto const state = [this]
    {
      auto const lock = std::scoped_lock{_implPtr->stateMutex};
      return State{
        .optTrack = _implPtr->optCurrentTrack,
        .positionMs =
          [this]
        {
          auto const frames = _implPtr->accumulatedFrames.load(std::memory_order_relaxed);
          auto const sr = _implPtr->engineSampleRate.load(std::memory_order_relaxed);
          return sr > 0 ? static_cast<std::uint32_t>((frames * 1000) / sr) : 0;
        }(),
        .wasPlaying = (_implPtr->status.transport == Transport::Playing),
      };
    }();

    stop();
    _implPtr->backendPtr = std::move(backendPtr);
    _implPtr->currentDevice = device;
    {
      auto const lock = std::scoped_lock{_implPtr->stateMutex};
      _implPtr->status = {};
      _implPtr->syncBackendIdentity();
      _implPtr->syncBackendStatus();
    }

    if (state.optTrack)
    {
      AUDIO_LOG_INFO("Resuming track '{}' after backend switch", state.optTrack->title);
      play(*state.optTrack);
      seek(state.positionMs);

      if (!state.wasPlaying)
      {
        pause();
      }
    }
  }

  void Engine::updateDevice(Device const& device)
  {
    _implPtr->currentDevice = device;
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
    auto const sourcePtr = _implPtr->source.load(std::memory_order_acquire);
    auto const lock = std::scoped_lock{_implPtr->stateMutex};
    auto snap = Engine::Status{_implPtr->status};
    auto const totalFrames = _implPtr->accumulatedFrames.load(std::memory_order_relaxed);
    auto const sampleRate = _implPtr->engineSampleRate.load(std::memory_order_relaxed);
    snap.positionMs = sampleRate > 0 ? static_cast<std::uint32_t>((totalFrames * 1000) / sampleRate) : 0;
    snap.routeState = _implPtr->routeTracker.state();
    snap.backendId = _implPtr->backendPtr->backendId();
    snap.bufferedMs = sourcePtr ? sourcePtr->bufferedMs() : 0;
    snap.underrunCount = _implPtr->underrunCount.load(std::memory_order_relaxed);
    return snap;
  }

  void Engine::play(TrackPlaybackDescriptor const& descriptor)
  {
    AUDIO_LOG_INFO("Play requested: {} - {} [{}]", descriptor.artist, descriptor.title, descriptor.filePath.string());

    _implPtr->backendPtr->stop();
    _implPtr->backendPtr->close();
    _implPtr->source.store({}, std::memory_order_release);

    auto sourcePtr = std::shared_ptr<ISource>{};
    auto backendFormat = Format{};

    {
      auto const lock = std::scoped_lock{_implPtr->stateMutex};
      _implPtr->underrunCount = 0;
      _implPtr->routeTracker.clear();
      _implPtr->backendStarted = false;
      _implPtr->playbackDrainPending = false;
      _implPtr->status.transport = Transport::Opening;
      _implPtr->optCurrentTrack = descriptor;
      _implPtr->syncBackendIdentity();
    }

    if (!_implPtr->openTrack(descriptor, sourcePtr, backendFormat))
    {
      auto const lock = std::scoped_lock{_implPtr->stateMutex};
      _implPtr->status.transport = Transport::Error;
      _implPtr->optCurrentTrack.reset();
      return;
    }

    {
      auto const lock = std::scoped_lock{_implPtr->stateMutex};
      _implPtr->status.transport = Transport::Buffering;
    }

    _implPtr->source.store(sourcePtr, std::memory_order_release);

    if (auto const openResult = _implPtr->backendPtr->open(backendFormat, _implPtr.get()); !openResult)
    {
      _implPtr->source.store({}, std::memory_order_release);
      auto const lock = std::scoped_lock{_implPtr->stateMutex};
      _implPtr->optCurrentTrack.reset();
      _implPtr->status.transport = Transport::Error;
      _implPtr->status.statusText = openResult.error().message;
      return;
    }

    {
      auto const lock = std::scoped_lock{_implPtr->stateMutex};
      _implPtr->syncBackendStatus();
    }

    auto const bufferedMs = sourcePtr ? sourcePtr->bufferedMs() : 0;

    if (auto const drained = !sourcePtr || sourcePtr->isDrained(); drained && bufferedMs == 0)
    {
      _implPtr->backendPtr->stop();
      _implPtr->backendPtr->close();
      _implPtr->source.store({}, std::memory_order_release);
      auto const lock = std::scoped_lock{_implPtr->stateMutex};
      _implPtr->resetEngine();
      return;
    }

    {
      auto const lock = std::scoped_lock{_implPtr->stateMutex};
      _implPtr->status.transport = Transport::Playing;
      _implPtr->backendStarted = true;
    }

    _implPtr->backendPtr->start();
  }

  void Engine::pause()
  {
    bool shouldPause = false;
    {
      auto const lock = std::scoped_lock{_implPtr->stateMutex};

      if (_implPtr->status.transport == Transport::Playing || _implPtr->status.transport == Transport::Buffering)
      {
        AUDIO_LOG_INFO("Playback paused");
        _implPtr->status.transport = Transport::Paused;
        shouldPause = _implPtr->backendStarted.load();
      }
    }

    if (shouldPause)
    {
      _implPtr->backendPtr->pause();
    }
  }

  void Engine::resume()
  {
    auto const srcPtr = _implPtr->source.load(std::memory_order_acquire);
    auto lock = std::unique_lock{_implPtr->stateMutex};

    if (_implPtr->status.transport != Transport::Paused)
    {
      return;
    }

    AUDIO_LOG_INFO("Playback resumed");

    if (_implPtr->backendStarted)
    {
      _implPtr->status.transport = Transport::Playing;
      lock.unlock();
      _implPtr->backendPtr->resume();
      return;
    }

    if (auto const drained = !srcPtr || srcPtr->isDrained(); drained && (srcPtr ? srcPtr->bufferedMs() : 0) == 0)
    {
      _implPtr->source.store({}, std::memory_order_release);
      _implPtr->resetEngine();
      return;
    }

    _implPtr->status.transport = Transport::Playing;
    _implPtr->backendStarted = true;
    lock.unlock();
    _implPtr->backendPtr->start();
  }

  void Engine::stop()
  {
    AUDIO_LOG_INFO("Playback stopped");
    _implPtr->backendPtr->stop();
    _implPtr->backendPtr->close();

    {
      auto const lock = std::scoped_lock{_implPtr->stateMutex};
      _implPtr->resetEngine();
    }

    _implPtr->source.store({}, std::memory_order_release);
  }

  void Engine::seek(std::uint32_t positionMs)
  {
    AUDIO_LOG_INFO("Seek requested: {} ms", positionMs);
    auto const sourcePtr = _implPtr->source.load(std::memory_order_acquire);

    if (!sourcePtr)
    {
      return;
    }

    bool wasPaused = false;
    {
      auto const lock = std::scoped_lock{_implPtr->stateMutex};
      wasPaused = (_implPtr->status.transport == Transport::Paused);
      _implPtr->status.transport = Transport::Buffering;
      _implPtr->status.positionMs = positionMs;
      auto const sr = _implPtr->engineSampleRate.load(std::memory_order_relaxed);
      _implPtr->accumulatedFrames.store(
        sr > 0 ? (static_cast<std::uint64_t>(positionMs) * sr) / 1000 : 0, std::memory_order_relaxed);
      _implPtr->status.statusText.clear();
    }

    _implPtr->backendPtr->stop();
    _implPtr->backendPtr->flush();
    _implPtr->backendStarted = false;
    _implPtr->playbackDrainPending = false;

    if (auto const seekResult = sourcePtr->seek(positionMs); !seekResult)
    {
      auto const lock = std::scoped_lock{_implPtr->stateMutex};
      _implPtr->status.transport = Transport::Error;
      _implPtr->status.statusText = seekResult.error().message;
      return;
    }

    auto const bufferedMs = sourcePtr->bufferedMs();

    if (auto const drained = sourcePtr->isDrained(); drained && bufferedMs == 0)
    {
      _implPtr->backendPtr->stop();
      _implPtr->backendPtr->close();
      _implPtr->source.store({}, std::memory_order_release);
      auto const lock = std::scoped_lock{_implPtr->stateMutex};
      _implPtr->resetEngine();
      return;
    }

    if (wasPaused)
    {
      auto const lock = std::scoped_lock{_implPtr->stateMutex};
      _implPtr->status.transport = Transport::Paused;
      return;
    }

    {
      auto const lock = std::scoped_lock{_implPtr->stateMutex};
      _implPtr->status.transport = Transport::Playing;
      _implPtr->backendStarted = true;
    }

    _implPtr->backendPtr->start();
  }

  void Engine::setVolume(float volume)
  {
    if (auto const result = _implPtr->backendPtr->set(props::kVolume, volume); !result)
    {
      AUDIO_LOG_ERROR("Failed to set volume: {}", result.error().message);
    }

    auto const lock = std::scoped_lock{_implPtr->stateMutex};
    _implPtr->status.volume = volume;
  }

  float Engine::volume() const
  {
    auto const lock = std::scoped_lock{_implPtr->stateMutex};
    return _implPtr->status.volume;
  }

  void Engine::setMuted(bool muted)
  {
    if (auto const result = _implPtr->backendPtr->set(props::kMuted, muted); !result)
    {
      AUDIO_LOG_ERROR("Failed to set muted state: {}", result.error().message);
    }

    auto const lock = std::scoped_lock{_implPtr->stateMutex};
    _implPtr->status.muted = muted;
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
