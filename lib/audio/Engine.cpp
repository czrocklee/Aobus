// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <ao/audio/Engine.h>
#include <ao/utility/Log.h>

#include <ao/audio/DecoderFactory.h>
#include <ao/audio/FormatNegotiator.h>
#include <ao/audio/IDecoderSession.h>
#include <ao/audio/MemorySource.h>
#include <ao/audio/StreamingSource.h>
#include <ao/utility/ByteView.h>

#include <algorithm>
#include <format>
#include <limits>
#include <ranges>
#include <set>
#include <sstream>

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

  Engine::Engine(std::unique_ptr<IBackend> backend,
                 Device const& device,
                 std::shared_ptr<ao::IMainThreadDispatcher> dispatcher,
                 DecoderFactoryFn decoderFactory)
    : _backend{std::move(backend)}
    , _dispatcher{std::move(dispatcher)}
    , _currentDevice{device}
    , _decoderFactory{std::move(decoderFactory)}
  {
    _status.backendId = _backend ? _backend->backendId() : kBackendNone;
    _status.profileId = _backend ? _backend->profileId() : ProfileId{};
    _status.currentDeviceId = _currentDevice.id;
  }

  Engine::~Engine()
  {
    stop();
  }

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
      auto const lock = std::lock_guard<std::mutex>{_stateMutex};

      return State{.track = _currentTrack,
                   .positionMs =
                     [this]()
                   {
                     auto const frames = _accumulatedFrames.load(std::memory_order_relaxed);
                     auto const sampleRate = _engineSampleRate.load(std::memory_order_relaxed);
                     return sampleRate > 0 ? static_cast<std::uint32_t>((frames * 1000) / sampleRate) : 0;
                   }(),
                   .wasPlaying = (_status.transport == Transport::Playing)};
    }();

    stop();

    _backend = std::move(backend);
    _currentDevice = device;

    {
      auto const lock = std::lock_guard<std::mutex>{_stateMutex};
      _status = {};
      _status.backendId = _backend ? _backend->backendId() : kBackendNone;
      _status.profileId = _backend ? _backend->profileId() : ProfileId{};
      _status.currentDeviceId = _currentDevice.id;
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
    _currentDevice = device;
  }

  void Engine::setOnTrackEnded(std::function<void()> callback)
  {
    auto const lock = std::lock_guard<std::mutex>{_stateMutex};
    _onTrackEnded = std::move(callback);
  }

  void Engine::setOnRouteChanged(OnRouteChanged callback)
  {
    auto const lock = std::lock_guard<std::mutex>{_stateMutex};
    _onRouteChanged = std::move(callback);
  }

  Engine::RouteStatus Engine::routeStatus() const
  {
    auto const lock = std::lock_guard<std::mutex>{_stateMutex};
    return _routeStatus;
  }

  void Engine::resetToIdle()
  {
    _currentTrack.reset();
    _backendStarted = false;
    _playbackDrainPending = false;
    _status = {};
    _status.backendId = _backend ? _backend->backendId() : kBackendNone;
    _status.profileId = _backend ? _backend->profileId() : ProfileId{};
    _status.currentDeviceId = _currentDevice.id;
    _accumulatedFrames.store(0, std::memory_order_relaxed);

    _routeStatus = {};
  }

  void Engine::play(TrackPlaybackDescriptor const& descriptor)
  {
    AUDIO_LOG_INFO("Play requested: {} - {} [{}]", descriptor.artist, descriptor.title, descriptor.filePath.string());

    if (_backend)
    {
      (*_backend).reset();
      _backend->stop();
      _backend->close();
    }

    _source.store({}, std::memory_order_release);

    auto callbacks = RenderCallbacks{};
    callbacks.userData = this;
    callbacks.readPcm = &Engine::onReadPcm;
    callbacks.isSourceDrained = &Engine::isSourceDrained;
    callbacks.onUnderrun = &Engine::onUnderrun;
    callbacks.onPositionAdvanced = &Engine::onPositionAdvanced;
    callbacks.onDrainComplete = &Engine::onDrainComplete;
    callbacks.onRouteReady = &Engine::onRouteReady;
    callbacks.onFormatChanged = &Engine::onFormatChanged;
    callbacks.onBackendError = &Engine::onBackendError;

    auto source = std::shared_ptr<ISource>{};
    auto backendFormat = Format{};

    {
      auto const lock = std::lock_guard<std::mutex>{_stateMutex};
      _underrunCount = 0;
      resetToIdle();
      _status.transport = Transport::Opening;
      _currentTrack = descriptor;

      if (!openTrack(descriptor, source, backendFormat))
      {
        _status.transport = Transport::Error;
        _currentTrack.reset();
        return;
      }

      _status.transport = Transport::Buffering;
    }

    _source.store(source, std::memory_order_release);

    if (_backend)
    {
      if (auto const openResult = _backend->open(backendFormat, callbacks); !openResult)
      {
        _source.store({}, std::memory_order_release);
        auto const lock = std::lock_guard<std::mutex>{_stateMutex};
        _currentTrack.reset();
        _status.transport = Transport::Error;
        _status.statusText = openResult.error().message;
        return;
      }
    }

    auto const bufferedMs = source ? source->bufferedMs() : 0;

    if (auto const drained = !source || source->isDrained(); drained && bufferedMs == 0)
    {
      if (_backend)
      {
        _backend->stop();
        _backend->close();
      }

      _source.store({}, std::memory_order_release);
      auto const lock = std::lock_guard<std::mutex>{_stateMutex};
      resetToIdle();
      return;
    }

    {
      auto const lock = std::lock_guard<std::mutex>{_stateMutex};
      _status.transport = Transport::Playing;
      _backendStarted = true;
    }

    if (_backend)
    {
      _backend->start();
    }
  }

  void Engine::pause()
  {
    bool shouldPause = false;

    {
      auto const lock = std::lock_guard<std::mutex>{_stateMutex};

      if (_status.transport == Transport::Playing || _status.transport == Transport::Buffering)
      {
        AUDIO_LOG_INFO("Playback paused");
        _status.transport = Transport::Paused;
        shouldPause = _backendStarted.load();
      }
    }

    if (shouldPause && _backend)
    {
      _backend->pause();
    }
  }

  void Engine::resume()
  {
    auto const source = _source.load(std::memory_order_acquire);
    auto lock = std::unique_lock<std::mutex>{_stateMutex};

    if (_status.transport != Transport::Paused)
    {
      return;
    }

    AUDIO_LOG_INFO("Playback resumed");

    if (_backendStarted)
    {
      _status.transport = Transport::Playing;
      lock.unlock();

      if (_backend)
      {
        _backend->resume();
      }

      return;
    }

    auto const bufferedMs = source ? source->bufferedMs() : 0;
    auto const drained = !source || source->isDrained();

    if (drained && bufferedMs == 0)
    {
      _source.store({}, std::memory_order_release);
      resetToIdle();
      return;
    }

    _status.transport = Transport::Playing;
    _backendStarted = true;
    lock.unlock();

    if (_backend)
    {
      _backend->start();
    }
  }

  void Engine::stop()
  {
    AUDIO_LOG_INFO("Playback stopped");

    if (_backend)
    {
      (*_backend).reset();
      _backend->stop();
      _backend->close();
    }

    _source.store({}, std::memory_order_release);
    auto const lock = std::lock_guard<std::mutex>{_stateMutex};
    resetToIdle();
  }

  void Engine::seek(std::uint32_t positionMs)
  {
    AUDIO_LOG_INFO("Seek requested: {} ms", positionMs);
    auto const source = _source.load(std::memory_order_acquire);

    if (!source)
    {
      return;
    }

    bool wasPaused = false;

    {
      auto const lock = std::lock_guard<std::mutex>{_stateMutex};
      wasPaused = (_status.transport == Transport::Paused);
      _status.transport = Transport::Buffering;
      _status.positionMs = positionMs;
      auto const sr = _engineSampleRate.load(std::memory_order_relaxed);
      _accumulatedFrames.store(
        sr > 0 ? (static_cast<std::uint64_t>(positionMs) * sr) / 1000 : 0, std::memory_order_relaxed);
      _status.statusText.clear();
    }

    if (_backend)
    {
      _backend->stop();
      _backend->flush();
    }

    _backendStarted = false;
    _playbackDrainPending = false;

    if (auto const seekResult = source->seek(positionMs); !seekResult)
    {
      auto const lock = std::lock_guard<std::mutex>{_stateMutex};
      _status.transport = Transport::Error;
      _status.statusText = seekResult.error().message;
      return;
    }

    auto const bufferedMs = source->bufferedMs();
    auto const drained = source->isDrained();

    if (drained && bufferedMs == 0)
    {
      if (_backend)
      {
        _backend->stop();
        _backend->close();
      }

      _source.store({}, std::memory_order_release);
      auto const lock = std::lock_guard<std::mutex>{_stateMutex};
      resetToIdle();
      return;
    }

    if (wasPaused)
    {
      auto const lock = std::lock_guard<std::mutex>{_stateMutex};
      _status.transport = Transport::Paused;
      return;
    }

    {
      auto const lock = std::lock_guard<std::mutex>{_stateMutex};
      _status.transport = Transport::Playing;
      _backendStarted = true;
    }

    if (_backend)
    {
      _backend->start();
    }
  }

  Engine::Status Engine::status() const
  {
    auto const source = _source.load(std::memory_order_acquire);
    auto const lock = std::lock_guard<std::mutex>{_stateMutex};
    auto snap = _status;
    auto const totalFrames = _accumulatedFrames.load(std::memory_order_relaxed);
    auto const sampleRate = _engineSampleRate.load(std::memory_order_relaxed);
    snap.positionMs = sampleRate > 0 ? static_cast<std::uint32_t>((totalFrames * 1000) / sampleRate) : 0;
    snap.backendId = _backend ? _backend->backendId() : kBackendNone;
    snap.bufferedMs = source ? source->bufferedMs() : 0;
    snap.underrunCount = _underrunCount.load(std::memory_order_relaxed);

    return snap;
  }

  void Engine::onBackendError(void* userData, std::string_view message) noexcept
  {
    auto* const self = ao::utility::unsafeDowncast<Engine>(userData);
    auto msg = std::string{message};

    if (self->_dispatcher)
    {
      self->_dispatcher->dispatch([self, msg = std::move(msg)]() { self->handleBackendError(msg); });
    }
    else
    {
      self->handleBackendError(msg);
    }
  }

  void Engine::handleBackendError(std::string_view message)
  {
    AUDIO_LOG_ERROR("Backend error: {}", message);

    // Stop immediately
    stop();

    auto const lock = std::lock_guard<std::mutex>{_stateMutex};
    _status.transport = Transport::Error;
    _status.statusText = std::string{message};
  }

  bool Engine::negotiateFormat(std::filesystem::path const& path,
                               DecodedStreamInfo const& info,
                               std::unique_ptr<IDecoderSession>& decoder,
                               Format& backendFormat)
  {
    if (!_backend)
    {
      return true;
    }

    auto const backendId = _backend->backendId();

    if (backendId == kBackendPipeWire && _backend->profileId() == kProfileShared)
    {
      backendFormat = info.outputFormat;
      AUDIO_LOG_INFO("PipeWire shared mode keeps the stream at {}Hz/{}b/{}ch",
                     backendFormat.sampleRate,
                     static_cast<int>(backendFormat.bitDepth),
                     static_cast<int>(backendFormat.channels));
      return true;
    }

    auto const plan = FormatNegotiator::buildPlan(info.sourceFormat, _currentDevice.capabilities);

    if (plan.requiresResample)
    {
      _status.statusText = std::format(
        "{} does not support {} Hz and RockStudio has no resampler yet", backendId, info.sourceFormat.sampleRate);
      return false;
    }

    if (plan.requiresChannelRemap)
    {
      _status.statusText = std::format("{} does not support {} channels and RockStudio has no channel remapper yet",
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

    // Re-open decoder if negotiated format differs
    if (!(plan.decoderOutputFormat == info.sourceFormat))
    {
      decoder->close();
      decoder = _decoderFactory ? _decoderFactory(path, plan.decoderOutputFormat)
                                : createDecoderSession(path, plan.decoderOutputFormat);

      if (!decoder)
      {
        _status.statusText = "Failed to re-open decoder with negotiated format";
        return false;
      }

      if (auto const reOpenResult = decoder->open(path); !reOpenResult)
      {
        _status.statusText = reOpenResult.error().message;
        return false;
      }
    }

    backendFormat = plan.deviceFormat;
    return true;
  }

  std::shared_ptr<ISource> Engine::createPcmSource(std::unique_ptr<IDecoderSession> decoder,
                                                   DecodedStreamInfo const& info)
  {
    if (shouldUseMemoryPcmSource(info))
    {
      auto const memorySource = std::make_shared<MemorySource>(std::move(decoder), info);

      if (auto const initResult = memorySource->initialize(); !initResult)
      {
        _status.statusText = initResult.error().message;
        return nullptr;
      }

      return memorySource;
    }

    auto const streamingSource = std::make_shared<StreamingSource>(
      std::move(decoder),
      info,
      [this](ao::Error const& err)
      {
        if (_dispatcher)
        {
          _dispatcher->dispatch([this, err]() { handleSourceError(err); });
        }
        else
        {
          handleSourceError(err);
        }
      },
      kPrerollTargetMs,
      kDecodeHighWatermarkMs);

    if (auto const initResult = streamingSource->initialize(); !initResult)
    {
      _status.statusText = initResult.error().message;
      return nullptr;
    }

    return streamingSource;
  }

  bool Engine::openTrack(TrackPlaybackDescriptor const& descriptor,
                         std::shared_ptr<ISource>& source,
                         Format& backendFormat)
  {
    auto const outputFormat = [&]
    {
      auto fmt = Format{};
      fmt.sampleRate = 0; // Use native
      fmt.channels = 0;   // Use native
      fmt.bitDepth = 0;   // Use native
      fmt.isFloat = false;
      fmt.isInterleaved = true;
      return fmt;
    }();

    auto decoder = _decoderFactory ? _decoderFactory(descriptor.filePath, outputFormat)
                                   : createDecoderSession(descriptor.filePath, outputFormat);

    if (decoder == nullptr)
    {
      _status.statusText = "No audio decoder backend is available";
      return false;
    }

    if (auto const openResult = decoder->open(descriptor.filePath); !openResult)
    {
      _status.statusText = openResult.error().message;
      return false;
    }

    auto info = decoder->streamInfo();

    if (info.outputFormat.sampleRate == 0 || info.outputFormat.channels == 0 || info.outputFormat.bitDepth == 0)
    {
      _status.statusText = "Decoder did not return a valid output format";
      return false;
    }

    if (!negotiateFormat(descriptor.filePath, info, decoder, backendFormat))
    {
      return false;
    }

    // Decoder might have been re-opened with a different output format
    info = decoder->streamInfo();

    if (source = createPcmSource(std::move(decoder), info); source == nullptr)
    {
      return false;
    }

    _status.durationMs = info.durationMs;
    _status.positionMs = 0;
    _accumulatedFrames.store(0, std::memory_order_relaxed);
    _status.flow = {};

    // Add initial Source (Decoder) Node
    _routeStatus.flow.nodes = {flow::Node{
                                 .id = "rs-decoder",
                                 .type = flow::NodeType::Decoder,
                                 .name = "Decoder",
                                 .optFormat = info.sourceFormat,
                               },
                               flow::Node{
                                 .id = "rs-engine",
                                 .type = flow::NodeType::Engine,
                                 .name = "Engine",
                                 .optFormat = info.outputFormat,
                               }};

    _routeStatus.flow.connections.clear();
    _routeStatus.flow.connections.push_back(
      flow::Connection{.sourceId = "rs-decoder", .destId = "rs-engine", .isActive = true});

    _engineSampleRate.store(info.outputFormat.sampleRate, std::memory_order_relaxed);

    _status.flow = _routeStatus.flow;

    return true;
  }

  std::size_t Engine::onReadPcm(void* userData, std::span<std::byte> output) noexcept
  {
    auto* const self = ao::utility::unsafeDowncast<Engine>(userData);
    auto const source = self->_source.load(std::memory_order_acquire);

    return source ? source->read(output) : 0;
  }

  bool Engine::isSourceDrained(void* userData) noexcept
  {
    auto* const self = ao::utility::unsafeDowncast<Engine>(userData);
    auto const source = self->_source.load(std::memory_order_acquire);

    if (!source)
    {
      return true;
    }

    auto const drained = source->isDrained();

    if (drained)
    {
      self->_playbackDrainPending = true;
    }

    return drained;
  }

  void Engine::onUnderrun(void* userData) noexcept
  {
    auto* const self = ao::utility::unsafeDowncast<Engine>(userData);
    ++self->_underrunCount;
  }

  void Engine::onPositionAdvanced(void* userData, std::uint32_t frames) noexcept
  {
    auto* const self = ao::utility::unsafeDowncast<Engine>(userData);
    self->_accumulatedFrames.fetch_add(frames, std::memory_order_relaxed);
  }

  void Engine::onDrainComplete(void* userData) noexcept
  {
    auto* const self = ao::utility::unsafeDowncast<Engine>(userData);

    if (!self->_playbackDrainPending.exchange(false, std::memory_order_relaxed))
    {
      return;
    }

    if (self->_dispatcher)
    {
      self->_dispatcher->dispatch([self]() { self->handleDrainComplete(); });
    }
    else
    {
      self->handleDrainComplete();
    }
  }

  void Engine::handleDrainComplete()
  {
    _source.store({}, std::memory_order_release);

    auto onTrackEnded = std::function<void()>{};
    auto onRouteChanged = OnRouteChanged{};

    {
      auto const lock = std::lock_guard<std::mutex>{_stateMutex};
      onRouteChanged = _onRouteChanged;
      resetToIdle();
      onTrackEnded = _onTrackEnded;
    }

    if (onRouteChanged)
    {
      if (_dispatcher)
      {
        _dispatcher->dispatch([cb = std::move(onRouteChanged)]() { cb({}); });
      }
      else
      {
        onRouteChanged({});
      }
    }

    // Callback OUTSIDE lock — allows play() to re-acquire _stateMutex
    if (onTrackEnded)
    {
      onTrackEnded();
    }
  }

  void Engine::onRouteReady(void* userData, std::string_view routeAnchor) noexcept
  {
    auto* const self = ao::utility::unsafeDowncast<Engine>(userData);
    auto anchor = std::string{routeAnchor};

    if (self->_dispatcher)
    {
      self->_dispatcher->dispatch([self, anchorCopy = std::move(anchor)]() { self->handleRouteReady(anchorCopy); });
    }
    else
    {
      self->handleRouteReady(anchor);
    }
  }

  void Engine::handleRouteReady(std::string_view routeAnchor)
  {
    auto cb = OnRouteChanged{};
    auto snap = RouteStatus{};

    {
      auto const lock = std::lock_guard<std::mutex>{_stateMutex};

      _routeStatus.optAnchor =
        RouteAnchor{.backend = _backend ? _backend->backendId() : kBackendNone, .id = std::string{routeAnchor}};

      cb = _onRouteChanged;
      snap = _routeStatus;
    }

    if (cb)
    {
      if (_dispatcher)
      {
        _dispatcher->dispatch([cb = std::move(cb), snap]() { cb(snap); });
      }
      else
      {
        cb(snap);
      }
    }
  }

  void Engine::handleSourceError(ao::Error const& error)
  {
    {
      auto const lock = std::lock_guard<std::mutex>{_stateMutex};

      if (_status.transport == Transport::Idle)
      {
        return;
      }

      _backendStarted = false;
      _playbackDrainPending = false;
      _status.transport = Transport::Error;
      _status.statusText = error.message.empty() ? "PCM source failed" : error.message;
    }

    // Backend call OUTSIDE lock to avoid holding _stateMutex during backend operations
    if (_backend)
    {
      _backend->stop();
    }
  }

  void Engine::onFormatChanged(void* userData, Format const& format) noexcept
  {
    auto* const self = ao::utility::unsafeDowncast<Engine>(userData);

    if (self->_dispatcher)
    {
      self->_dispatcher->dispatch([self, format]() { self->handleFormatChanged(format); });
    }
    else
    {
      self->handleFormatChanged(format);
    }
  }

  void Engine::handleFormatChanged(Format const& format)
  {
    auto cb = OnRouteChanged{};
    auto snap = RouteStatus{};

    {
      auto const lock = std::lock_guard<std::mutex>{_stateMutex};

      // Update engine node in the route snapshot
      for (auto& node : _routeStatus.flow.nodes)
      {
        if (node.id == "rs-engine")
        {
          node.optFormat = format;
          break;
        }
      }

      // Update engine node in the public status
      for (auto& node : _status.flow.nodes)
      {
        if (node.id == "rs-engine")
        {
          node.optFormat = format;
          break;
        }
      }

      cb = _onRouteChanged;
      snap = _routeStatus;
    }

    if (cb)
    {
      if (_dispatcher)
      {
        _dispatcher->dispatch([cb = std::move(cb), snap]() { cb(snap); });
      }
      else
      {
        cb(snap);
      }
    }
  }
} // namespace ao::audio
