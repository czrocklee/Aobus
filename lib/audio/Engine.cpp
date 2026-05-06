// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/audio/DecoderFactory.h>
#include <ao/audio/Engine.h>
#include <ao/audio/FormatNegotiator.h>
#include <ao/audio/IBackend.h>
#include <ao/audio/IDecoderSession.h>
#include <ao/audio/ISource.h>
#include <ao/audio/MemorySource.h>
#include <ao/audio/StreamingSource.h>
#include <ao/utility/Log.h>

#include <format>

namespace ao::audio
{
  namespace
  {
    constexpr std::uint32_t kPrerollTargetMs = 200;
    constexpr std::uint32_t kDecodeHighWatermarkMs = 750;
    constexpr std::uint64_t kMemoryPcmSourceBudgetBytes = 64ULL * 1024ULL * 1024ULL;
    constexpr std::uint8_t kBytesPer24BitSample = 3;

    std::uint64_t bytesPerSecond(Format const& format) noexcept
    {
      if (format.sampleRate == 0 || format.channels == 0 || format.bitDepth == 0)
      {
        return 0;
      }

      auto bytesPerSample = 2U;

      if (format.bitDepth == 24U)
      {
        bytesPerSample = kBytesPer24BitSample;
      }
      else if (format.bitDepth > 16U)
      {
        bytesPerSample = 4U;
      }

      return static_cast<std::uint64_t>(format.sampleRate) * format.channels * bytesPerSample;
    }

    std::uint64_t estimatedDecodedBytes(DecodedStreamInfo const& info) noexcept
    {
      auto const rate = bytesPerSecond(info.outputFormat);

      if (rate == 0 || info.durationMs == 0)
      {
        return 0;
      }

      return (static_cast<std::uint64_t>(info.durationMs) * rate) / 1000U;
    }

    bool shouldUseMemoryPcmSource(DecodedStreamInfo const& info) noexcept
    {
      auto const decodedBytes = estimatedDecodedBytes(info);
      return decodedBytes > 0 && decodedBytes <= kMemoryPcmSourceBudgetBytes;
    }
  } // namespace

  // ── Engine::Impl: data bucket + callbacks + handlers ────────────

  struct Engine::Impl final
  {
    std::unique_ptr<IBackend> backend;
    Device currentDevice;

    std::atomic<std::shared_ptr<ISource>> source;
    std::atomic<bool> backendStarted{false};
    std::atomic<bool> playbackDrainPending{false};
    std::atomic<std::uint32_t> underrunCount{0};
    std::atomic<std::uint64_t> accumulatedFrames{0};
    std::atomic<std::uint32_t> engineSampleRate{0};

    mutable std::mutex stateMutex;
    std::optional<TrackPlaybackDescriptor> currentTrack;
    Engine::Status status;
    std::function<void()> onTrackEnded;
    Engine::OnRouteChanged onRouteChanged;
    detail::RouteTracker routeTracker;
    DecoderFactoryFn decoderFactory;

    RenderCallbacks renderCallbacks;

    // ── Construction ──────────────────────────────────────────────
    Impl(std::unique_ptr<IBackend> backend, Device const& device, DecoderFactoryFn decoderFactory)
      : backend{std::move(backend)}, currentDevice{device}, decoderFactory{std::move(decoderFactory)}
    {
      syncBackendIdentity();
      renderCallbacks = RenderCallbacks{.userData = this,
                                        .readPcm = &Impl::onReadPcm,
                                        .isSourceDrained = &Impl::isSourceDrained,
                                        .onUnderrun = &Impl::onUnderrun,
                                        .onPositionAdvanced = &Impl::onPositionAdvanced,
                                        .onDrainComplete = &Impl::onDrainComplete,
                                        .onRouteReady = &Impl::onRouteReady,
                                        .onFormatChanged = &Impl::onFormatChanged,
                                        .onPropertyChanged = &Impl::onPropertyChanged,
                                        .onBackendError = &Impl::onBackendError};
    }

    // ── Helpers ────────────────────────────────────────────────────
    void syncBackendIdentity()
    {
      status.backendId = backend->backendId();
      status.profileId = backend->profileId();
      status.currentDeviceId = currentDevice.id;
    }

    void syncBackendStatus()
    {
      if (auto const vol = backend->get(props::Volume))
      {
        status.volume = *vol;
      }

      if (auto const mute = backend->get(props::Muted))
      {
        status.muted = *mute;
      }

      status.volumeAvailable = backend->queryProperty(PropertyId::Volume).isAvailable;
    }

    void resetEngine()
    {
      currentTrack.reset();
      backendStarted = false;
      playbackDrainPending = false;
      status = {};
      syncBackendIdentity();
      syncBackendStatus();
      accumulatedFrames.store(0, std::memory_order_relaxed);
      routeTracker.clear();
    }

    // ── Callbacks ──────────────────────────────────────────────────
    static std::size_t onReadPcm(void* userData, std::span<std::byte> output) noexcept
    {
      auto* const impl = static_cast<Impl*>(userData);
      auto const source = impl->source.load(std::memory_order_acquire);
      return source ? source->read(output) : 0;
    }

    static bool isSourceDrained(void* userData) noexcept
    {
      auto* const impl = static_cast<Impl*>(userData);
      auto const source = impl->source.load(std::memory_order_acquire);

      if (!source)
      {
        return true;
      }

      if (source->isDrained())
      {
        impl->playbackDrainPending = true;
        return true;
      }

      return false;
    }

    static void onUnderrun(void* userData) noexcept { ++static_cast<Impl*>(userData)->underrunCount; }

    static void onPositionAdvanced(void* userData, std::uint32_t frames) noexcept
    {
      static_cast<Impl*>(userData)->accumulatedFrames.fetch_add(frames, std::memory_order_relaxed);
    }

    static void onDrainComplete(void* userData) noexcept
    {
      auto* const impl = static_cast<Impl*>(userData);

      if (!impl->playbackDrainPending.exchange(false, std::memory_order_relaxed))
      {
        return;
      }

      impl->handleDrainComplete();
    }

    static void onRouteReady(void* userData, std::string_view routeAnchor) noexcept
    {
      auto* const impl = static_cast<Impl*>(userData);
      auto anchor = std::string{routeAnchor};
      impl->handleRouteReady(anchor);
    }

    static void onFormatChanged(void* userData, Format const& format) noexcept
    {
      auto* const impl = static_cast<Impl*>(userData);
      impl->handleFormatChanged(format);
    }

    static void onPropertyChanged(void* userData, PropertyId id) noexcept
    {
      auto* const impl = static_cast<Impl*>(userData);
      impl->handlePropertyChanged(id);
    }

    static void onBackendError(void* userData, std::string_view message) noexcept
    {
      auto* const impl = static_cast<Impl*>(userData);
      auto msg = std::string{message};
      impl->handleBackendError(msg);
    }

    // ── Handlers ───────────────────────────────────────────────────
    void handleBackendError(std::string_view message)
    {
      AUDIO_LOG_ERROR("Backend error: {}", message);
      backend->stop();
      backend->close();
      source.store({}, std::memory_order_release);
      auto const lock = std::lock_guard{stateMutex};
      resetEngine();
      status.transport = Transport::Error;
      status.statusText = std::string{message};
    }

    void handleSourceError(ao::Error const& error)
    {
      {
        auto const lock = std::lock_guard{stateMutex};

        if (status.transport == Transport::Idle)
        {
          return;
        }

        backendStarted = false;
        playbackDrainPending = false;
        status.transport = Transport::Error;
        status.statusText = error.message.empty() ? "PCM source failed" : error.message;
      }

      backend->stop();
    }

    void notifyRouteChanged()
    {
      auto callback = Engine::OnRouteChanged{};
      auto snapshot = Engine::RouteStatus{};
      {
        auto const lock = std::lock_guard{stateMutex};
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
      routeTracker.setAnchor(backend->backendId(), std::string{routeAnchor});
      notifyRouteChanged();
    }

    void handleFormatChanged(Format const& format)
    {
      {
        auto const lock = std::lock_guard{stateMutex};
        routeTracker.setEngineFormat(format);
        status.routeState.engineOutputFormat = format;
      }
      notifyRouteChanged();
    }

    void handlePropertyChanged(PropertyId id)
    {
      {
        auto const lock = std::lock_guard{stateMutex};
        if (id == PropertyId::Volume)
        {
          if (auto const vol = backend->get(props::Volume))
          {
            status.volume = *vol;
          }
        }
        else if (id == PropertyId::Muted)
        {
          if (auto const mute = backend->get(props::Muted))
          {
            status.muted = *mute;
          }
        }
        status.volumeAvailable = backend->queryProperty(PropertyId::Volume).isAvailable;
      }
      notifyRouteChanged();
    }

    void handleDrainComplete()
    {
      source.store({}, std::memory_order_release);

      auto onTrackEndedCallback = std::function<void()>{};
      auto onRouteChangedCallback = Engine::OnRouteChanged{};

      {
        auto const lock = std::lock_guard{stateMutex};
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
    bool negotiateFormat(std::filesystem::path const& path,
                         DecodedStreamInfo const& info,
                         std::unique_ptr<IDecoderSession>& decoder,
                         Format& backendFormat)
    {
      auto const backendId = backend->backendId();

      if (backendId == kBackendPipeWire && backend->profileId() == kProfileShared)
      {
        backendFormat = info.outputFormat;
        AUDIO_LOG_INFO("PipeWire shared mode keeps the stream at {}Hz/{}b/{}ch",
                       backendFormat.sampleRate,
                       static_cast<int>(backendFormat.bitDepth),
                       static_cast<int>(backendFormat.channels));

        return true;
      }

      auto const plan = FormatNegotiator::buildPlan(info.sourceFormat, currentDevice.capabilities);

      if (plan.requiresResample)
      {
        status.statusText = std::format(
          "{} does not support {} Hz and Aobus has no resampler yet", backendId, info.sourceFormat.sampleRate);

        return false;
      }

      if (plan.requiresChannelRemap)
      {
        status.statusText = std::format("{} does not support {} channels and Aobus has no channel remapper yet",
                                        backendId,
                                        static_cast<int>(info.sourceFormat.channels));

        return false;
      }

      AUDIO_LOG_INFO("Negotiated Plan: decoder={}b/{}bits, device={}Hz/{}b, reason: {}",
                     static_cast<int>(plan.decoderOutputFormat.bitDepth),
                     static_cast<int>(plan.decoderOutputFormat.validBits),
                     plan.deviceFormat.sampleRate,
                     static_cast<int>(plan.deviceFormat.bitDepth),
                     plan.reason);

      if (!(plan.decoderOutputFormat == info.sourceFormat))
      {
        decoder->close();
        decoder = decoderFactory ? decoderFactory(path, plan.decoderOutputFormat)
                                 : createDecoderSession(path, plan.decoderOutputFormat);

        if (!decoder)
        {
          status.statusText = "Failed to re-open decoder with negotiated format";
          return false;
        }

        if (auto const reOpenResult = decoder->open(path); !reOpenResult)
        {
          status.statusText = reOpenResult.error().message;
          return false;
        }
      }

      backendFormat = plan.deviceFormat;
      return true;
    }

    std::shared_ptr<ISource> createPcmSource(std::unique_ptr<IDecoderSession> decoder, DecodedStreamInfo const& info)
    {
      if (shouldUseMemoryPcmSource(info))
      {
        auto const memorySource = std::make_shared<MemorySource>(std::move(decoder), info);

        if (auto const initResult = memorySource->initialize(); !initResult)
        {
          status.statusText = initResult.error().message;
          return nullptr;
        }

        return memorySource;
      }

      auto const streamingSource = std::make_shared<StreamingSource>(
        std::move(decoder),
        info,
        [this](ao::Error const& err) { handleSourceError(err); },
        kPrerollTargetMs,
        kDecodeHighWatermarkMs);

      if (auto const initResult = streamingSource->initialize(); !initResult)
      {
        status.statusText = initResult.error().message;
        return nullptr;
      }

      return streamingSource;
    }

    bool openTrack(TrackPlaybackDescriptor const& descriptor, std::shared_ptr<ISource>& source, Format& backendFormat)
    {
      auto const outputFormat = []() { return Format{.isInterleaved = true}; }();

      auto decoder = decoderFactory ? decoderFactory(descriptor.filePath, outputFormat)
                                    : createDecoderSession(descriptor.filePath, outputFormat);

      if (decoder == nullptr)
      {
        status.statusText = "No audio decoder backend is available";
        return false;
      }

      if (auto const openResult = decoder->open(descriptor.filePath); !openResult)
      {
        status.statusText = openResult.error().message;
        return false;
      }

      auto info = decoder->streamInfo();

      if (info.outputFormat.sampleRate == 0 || info.outputFormat.channels == 0 || info.outputFormat.bitDepth == 0)
      {
        status.statusText = "Decoder did not return a valid output format";
        return false;
      }

      if (!negotiateFormat(descriptor.filePath, info, decoder, backendFormat))
      {
        return false;
      }

      info = decoder->streamInfo();

      if (source = createPcmSource(std::move(decoder), info); source == nullptr)
      {
        return false;
      }

      status.durationMs = info.durationMs;
      status.positionMs = 0;
      accumulatedFrames.store(0, std::memory_order_relaxed);
      routeTracker.setDecoder(info.sourceFormat, info.outputFormat, info.isLossy);
      routeTracker.setEngineFormat(info.outputFormat);
      status.routeState = routeTracker.state();
      engineSampleRate.store(info.outputFormat.sampleRate, std::memory_order_relaxed);

      return true;
    }
  };

  // ── Engine ──────────────────────────────────────────────────────

  Engine::Engine(std::unique_ptr<IBackend> backend, Device const& device, DecoderFactoryFn decoderFactory)
    : _impl{std::make_unique<Impl>(std::move(backend), device, std::move(decoderFactory))}
  {
    _impl->syncBackendStatus();
  }

  Engine::~Engine() = default;

  void Engine::setBackend(std::unique_ptr<IBackend> backend, Device const& device)
  {
    struct State
    {
      std::optional<TrackPlaybackDescriptor> track;
      std::uint32_t positionMs = 0;
      bool wasPlaying = false;
    };

    auto const state = [this]()
    {
      auto const lock = std::lock_guard{_impl->stateMutex};
      return State{
        .track = _impl->currentTrack,
        .positionMs =
          [this]()
        {
          auto const frames = _impl->accumulatedFrames.load(std::memory_order_relaxed);
          auto const sr = _impl->engineSampleRate.load(std::memory_order_relaxed);
          return sr > 0 ? static_cast<std::uint32_t>((frames * 1000) / sr) : 0;
        }(),
        .wasPlaying = (_impl->status.transport == Transport::Playing),
      };
    }();

    stop();
    _impl->backend = std::move(backend);
    _impl->currentDevice = device;
    {
      auto const lock = std::lock_guard{_impl->stateMutex};
      _impl->status = {};
      _impl->syncBackendIdentity();
      _impl->syncBackendStatus();
    }

    if (state.track)
    {
      AUDIO_LOG_INFO("Resuming track '{}' after backend switch", state.track->title);
      play(*state.track);
      seek(state.positionMs);

      if (!state.wasPlaying)
      {
        pause();
      }
    }
  }

  void Engine::updateDevice(Device const& device)
  {
    _impl->currentDevice = device;
  }

  void Engine::setOnTrackEnded(std::function<void()> callback)
  {
    auto const lock = std::lock_guard{_impl->stateMutex};
    _impl->onTrackEnded = std::move(callback);
  }

  void Engine::setOnRouteChanged(OnRouteChanged callback)
  {
    auto const lock = std::lock_guard{_impl->stateMutex};
    _impl->onRouteChanged = std::move(callback);
  }

  Engine::RouteStatus Engine::routeStatus() const
  {
    return RouteStatus{.state = _impl->routeTracker.state(), .optAnchor = _impl->routeTracker.anchor()};
  }

  Transport Engine::transport() const
  {
    auto const lock = std::lock_guard{_impl->stateMutex};
    return _impl->status.transport;
  }

  BackendId Engine::backendId() const
  {
    auto const lock = std::lock_guard{_impl->stateMutex};
    return _impl->status.backendId;
  }

  Engine::Status Engine::status() const
  {
    auto const source = _impl->source.load(std::memory_order_acquire);
    auto const lock = std::lock_guard{_impl->stateMutex};
    auto snap = _impl->status;
    auto const totalFrames = _impl->accumulatedFrames.load(std::memory_order_relaxed);
    auto const sampleRate = _impl->engineSampleRate.load(std::memory_order_relaxed);
    snap.positionMs = sampleRate > 0 ? static_cast<std::uint32_t>((totalFrames * 1000) / sampleRate) : 0;
    snap.routeState = _impl->routeTracker.state();
    snap.backendId = _impl->backend->backendId();
    snap.bufferedMs = source ? source->bufferedMs() : 0;
    snap.underrunCount = _impl->underrunCount.load(std::memory_order_relaxed);
    return snap;
  }

  void Engine::play(TrackPlaybackDescriptor const& descriptor)
  {
    AUDIO_LOG_INFO("Play requested: {} - {} [{}]", descriptor.artist, descriptor.title, descriptor.filePath.string());

    _impl->backend->stop();
    _impl->backend->close();
    _impl->source.store({}, std::memory_order_release);

    auto source = std::shared_ptr<ISource>{};
    auto backendFormat = Format{};

    {
      auto const lock = std::lock_guard{_impl->stateMutex};
      _impl->underrunCount = 0;
      _impl->routeTracker.clear();
      _impl->backendStarted = false;
      _impl->playbackDrainPending = false;
      _impl->status.transport = Transport::Opening;
      _impl->currentTrack = descriptor;
      _impl->syncBackendIdentity();

      if (!_impl->openTrack(descriptor, source, backendFormat))
      {
        _impl->status.transport = Transport::Error;
        _impl->currentTrack.reset();
        return;
      }

      _impl->status.transport = Transport::Buffering;
    }

    _impl->source.store(source, std::memory_order_release);

    if (auto const openResult = _impl->backend->open(backendFormat, _impl->renderCallbacks); !openResult)
    {
      _impl->source.store({}, std::memory_order_release);
      auto const lock = std::lock_guard{_impl->stateMutex};
      _impl->currentTrack.reset();
      _impl->status.transport = Transport::Error;
      _impl->status.statusText = openResult.error().message;
      return;
    }

    {
      auto const lock = std::lock_guard{_impl->stateMutex};
      _impl->syncBackendStatus();
    }

    auto const bufferedMs = source ? source->bufferedMs() : 0;
    if (auto const drained = !source || source->isDrained(); drained && bufferedMs == 0)
    {
      _impl->backend->stop();
      _impl->backend->close();
      _impl->source.store({}, std::memory_order_release);
      auto const lock = std::lock_guard{_impl->stateMutex};
      _impl->resetEngine();
      return;
    }

    {
      auto const lock = std::lock_guard{_impl->stateMutex};
      _impl->status.transport = Transport::Playing;
      _impl->backendStarted = true;
    }
    _impl->backend->start();
  }

  void Engine::pause()
  {
    bool shouldPause = false;
    {
      auto const lock = std::lock_guard{_impl->stateMutex};
      if (_impl->status.transport == Transport::Playing || _impl->status.transport == Transport::Buffering)
      {
        AUDIO_LOG_INFO("Playback paused");
        _impl->status.transport = Transport::Paused;
        shouldPause = _impl->backendStarted.load();
      }
    }
    if (shouldPause)
    {
      _impl->backend->pause();
    }
  }

  void Engine::resume()
  {
    auto const src = _impl->source.load(std::memory_order_acquire);
    auto lock = std::unique_lock{_impl->stateMutex};

    if (_impl->status.transport != Transport::Paused)
    {
      return;
    }

    AUDIO_LOG_INFO("Playback resumed");

    if (_impl->backendStarted)
    {
      _impl->status.transport = Transport::Playing;
      lock.unlock();
      _impl->backend->resume();
      return;
    }

    if (auto const drained = !src || src->isDrained(); drained && (src ? src->bufferedMs() : 0) == 0)
    {
      _impl->source.store({}, std::memory_order_release);
      _impl->resetEngine();
      return;
    }

    _impl->status.transport = Transport::Playing;
    _impl->backendStarted = true;
    lock.unlock();
    _impl->backend->start();
  }

  void Engine::stop()
  {
    AUDIO_LOG_INFO("Playback stopped");
    _impl->backend->stop();
    _impl->backend->close();
    _impl->source.store({}, std::memory_order_release);
    auto const lock = std::lock_guard{_impl->stateMutex};
    _impl->resetEngine();
  }

  void Engine::seek(std::uint32_t positionMs)
  {
    AUDIO_LOG_INFO("Seek requested: {} ms", positionMs);
    auto const source = _impl->source.load(std::memory_order_acquire);
    if (!source)
    {
      return;
    }

    bool wasPaused = false;
    {
      auto const lock = std::lock_guard{_impl->stateMutex};
      wasPaused = (_impl->status.transport == Transport::Paused);
      _impl->status.transport = Transport::Buffering;
      _impl->status.positionMs = positionMs;
      auto const sr = _impl->engineSampleRate.load(std::memory_order_relaxed);
      _impl->accumulatedFrames.store(
        sr > 0 ? (static_cast<std::uint64_t>(positionMs) * sr) / 1000 : 0, std::memory_order_relaxed);
      _impl->status.statusText.clear();
    }

    _impl->backend->stop();
    _impl->backend->flush();
    _impl->backendStarted = false;
    _impl->playbackDrainPending = false;

    if (auto const seekResult = source->seek(positionMs); !seekResult)
    {
      auto const lock = std::lock_guard{_impl->stateMutex};
      _impl->status.transport = Transport::Error;
      _impl->status.statusText = seekResult.error().message;
      return;
    }

    auto const bufferedMs = source->bufferedMs();
    if (auto const drained = source->isDrained(); drained && bufferedMs == 0)
    {
      _impl->backend->stop();
      _impl->backend->close();
      _impl->source.store({}, std::memory_order_release);
      auto const lock = std::lock_guard{_impl->stateMutex};
      _impl->resetEngine();
      return;
    }

    if (wasPaused)
    {
      auto const lock = std::lock_guard{_impl->stateMutex};
      _impl->status.transport = Transport::Paused;
      return;
    }

    {
      auto const lock = std::lock_guard{_impl->stateMutex};
      _impl->status.transport = Transport::Playing;
      _impl->backendStarted = true;
    }
    _impl->backend->start();
  }

  void Engine::setVolume(float volume)
  {
    if (auto const result = _impl->backend->set(props::Volume, volume); !result)
    {
      AUDIO_LOG_ERROR("Failed to set volume: {}", result.error().message);
    }
    auto const lock = std::lock_guard{_impl->stateMutex};
    _impl->status.volume = volume;
  }

  float Engine::getVolume() const
  {
    auto const lock = std::lock_guard{_impl->stateMutex};
    return _impl->status.volume;
  }

  void Engine::setMuted(bool muted)
  {
    if (auto const result = _impl->backend->set(props::Muted, muted); !result)
    {
      AUDIO_LOG_ERROR("Failed to set muted state: {}", result.error().message);
    }
    auto const lock = std::lock_guard{_impl->stateMutex};
    _impl->status.muted = muted;
  }

  bool Engine::isMuted() const
  {
    auto const lock = std::lock_guard{_impl->stateMutex};
    return _impl->status.muted;
  }

  bool Engine::isVolumeAvailable() const
  {
    auto const lock = std::lock_guard{_impl->stateMutex};
    return _impl->status.volumeAvailable;
  }
} // namespace ao::audio
