// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <rs/audio/PlaybackEngine.h>
#include <rs/utility/Log.h>

#include <rs/audio/AudioDecoderFactory.h>
#include <rs/audio/FormatNegotiator.h>
#include <rs/audio/IAudioDecoderSession.h>
#include <rs/audio/MemoryPcmSource.h>
#include <rs/audio/StreamingPcmSource.h>

#include <algorithm>
#include <format>
#include <limits>
#include <ranges>
#include <rs/utility/ByteView.h>
#include <set>
#include <sstream>

namespace rs::audio
{

  namespace
  {
    constexpr std::uint32_t kPrerollTargetMs = 200;
    constexpr std::uint32_t kDecodeHighWatermarkMs = 750;
    constexpr std::uint64_t kMemoryPcmSourceBudgetBytes = 64ULL * 1024ULL * 1024ULL;
    constexpr std::uint8_t kBytesPer24BitSample = 3;

    std::uint64_t bytesPerSecond(AudioFormat const& format) noexcept
    {
      if (format.sampleRate == 0 || format.channels == 0 || format.bitDepth == 0)
      {
        return 0;
      }

      std::uint32_t bytesPerSample = 2U;

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

    std::uint64_t estimatedDecodedBytes(rs::audio::DecodedStreamInfo const& info) noexcept
    {
      auto const rate = bytesPerSecond(info.outputFormat);
      if (rate == 0 || info.durationMs == 0)
      {
        return 0;
      }
      return (static_cast<std::uint64_t>(info.durationMs) * rate) / 1000U;
    }

    bool shouldUseMemoryPcmSource(rs::audio::DecodedStreamInfo const& info) noexcept
    {
      auto const decodedBytes = estimatedDecodedBytes(info);
      return decodedBytes > 0 && decodedBytes <= kMemoryPcmSourceBudgetBytes;
    }
  } // namespace

  PlaybackEngine::PlaybackEngine(std::unique_ptr<rs::audio::IAudioBackend> backend,
                                 rs::audio::AudioDevice const& device,
                                 std::shared_ptr<rs::IMainThreadDispatcher> dispatcher)
    : _backend{std::move(backend)}, _dispatcher{std::move(dispatcher)}, _currentDevice{device}
  {
    _snapshot.backend = _backend ? _backend->kind() : BackendKind::None;
  }

  PlaybackEngine::~PlaybackEngine()
  {
    stop();
  }

  void PlaybackEngine::setBackend(std::unique_ptr<rs::audio::IAudioBackend> backend,
                                  rs::audio::AudioDevice const& device)
  {
    struct State
    {
      std::optional<TrackPlaybackDescriptor> track;
      std::uint32_t positionMs = 0;
      bool wasPlaying = false;
    };

    auto const state = [this]()
    {
      auto lock = std::lock_guard<std::mutex>{_stateMutex};

      return State{.track = _currentTrack,
                   .positionMs = _snapshot.positionMs,
                   .wasPlaying = (_snapshot.state == TransportState::Playing)};
    }();

    stop();

    _backend = std::move(backend);
    _currentDevice = device;

    {
      auto lock = std::lock_guard<std::mutex>{_stateMutex};
      _snapshot.backend = _backend ? _backend->kind() : BackendKind::None;
      _snapshot.currentDeviceId = _currentDevice.id;
    }

    if (state.track)
    {
      PLAYBACK_LOG_INFO("Resuming track '{}' after backend switch", state.track->title);
      play(*state.track);
      seek(state.positionMs);

      if (!state.wasPlaying)
      {
        pause();
      }
    }
  }

  void PlaybackEngine::setOnTrackEnded(std::function<void()> callback)
  {
    auto lock = std::lock_guard<std::mutex>{_stateMutex};
    _onTrackEnded = std::move(callback);
  }

  void PlaybackEngine::setOnRouteChanged(OnRouteChanged callback)
  {
    auto lock = std::lock_guard<std::mutex>{_stateMutex};
    _onRouteChanged = std::move(callback);
  }

  EngineRouteSnapshot PlaybackEngine::routeSnapshot() const
  {
    auto lock = std::lock_guard<std::mutex>{_stateMutex};
    return _routeSnapshot;
  }

  void PlaybackEngine::resetToIdle()
  {
    _currentTrack.reset();
    _backendStarted = false;
    _playbackDrainPending = false;
    _snapshot = {};
    _snapshot.backend = _backend ? _backend->kind() : BackendKind::None;

    _routeSnapshot = {};

    if (_onRouteChanged)
    {
      auto const snap = _routeSnapshot;

      if (_dispatcher)
      {
        _dispatcher->dispatch([this, snap]() { _onRouteChanged(snap); });
      }
      else
      {
        _onRouteChanged(snap);
      }
    }
  }

  void PlaybackEngine::play(TrackPlaybackDescriptor const& descriptor)
  {
    PLAYBACK_LOG_INFO(
      "Play requested: {} - {} [{}]", descriptor.artist, descriptor.title, descriptor.filePath.string());

    if (_backend)
    {
      (*_backend).reset();
      _backend->stop();
      _backend->close();
    }

    _source.store({}, std::memory_order_release);

    auto callbacks = rs::audio::AudioRenderCallbacks{};
    callbacks.userData = this;
    callbacks.readPcm = &PlaybackEngine::onReadPcm;
    callbacks.isSourceDrained = &PlaybackEngine::isSourceDrained;
    callbacks.onUnderrun = &PlaybackEngine::onUnderrun;
    callbacks.onPositionAdvanced = &PlaybackEngine::onPositionAdvanced;
    callbacks.onDrainComplete = &PlaybackEngine::onDrainComplete;
    callbacks.onRouteReady = &PlaybackEngine::onRouteReady;
    callbacks.onFormatChanged = &PlaybackEngine::onFormatChanged;
    callbacks.onBackendError = &PlaybackEngine::onBackendError;

    auto source = std::shared_ptr<rs::audio::IPcmSource>{};
    auto backendFormat = AudioFormat{};

    {
      auto lock = std::lock_guard<std::mutex>{_stateMutex};
      _underrunCount = 0;
      resetToIdle();
      _snapshot.state = TransportState::Opening;
      _snapshot.trackTitle = descriptor.title;
      _snapshot.trackArtist = descriptor.artist;
      _currentTrack = descriptor;

      if (!openTrack(descriptor, source, backendFormat))
      {
        _snapshot.state = TransportState::Error;
        _currentTrack.reset();
        return;
      }

      _snapshot.state = TransportState::Buffering;
    }

    _source.store(source, std::memory_order_release);

    if (_backend)
    {
      if (auto const openResult = _backend->open(backendFormat, callbacks); !openResult)
      {
        _source.store({}, std::memory_order_release);
        auto lock = std::lock_guard<std::mutex>{_stateMutex};
        _currentTrack.reset();
        _snapshot.state = TransportState::Error;
        _snapshot.statusText = openResult.error().message;
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
      auto lock = std::lock_guard<std::mutex>{_stateMutex};
      resetToIdle();
      return;
    }

    {
      auto lock = std::lock_guard<std::mutex>{_stateMutex};
      _snapshot.state = TransportState::Playing;
      _backendStarted = true;
    }

    if (_backend)
    {
      _backend->start();
    }
  }

  void PlaybackEngine::pause()
  {
    bool shouldPause = false;

    {
      auto lock = std::lock_guard<std::mutex>{_stateMutex};

      if (_snapshot.state == TransportState::Playing || _snapshot.state == TransportState::Buffering)
      {
        PLAYBACK_LOG_INFO("Playback paused");
        _snapshot.state = TransportState::Paused;
        shouldPause = _backendStarted.load();
      }
    }

    if (shouldPause && _backend)
    {
      _backend->pause();
    }
  }

  void PlaybackEngine::resume()
  {
    auto const source = _source.load(std::memory_order_acquire);
    auto lock = std::unique_lock<std::mutex>{_stateMutex};

    if (_snapshot.state != TransportState::Paused)
    {
      return;
    }

    PLAYBACK_LOG_INFO("Playback resumed");

    if (_backendStarted)
    {
      _snapshot.state = TransportState::Playing;
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

    _snapshot.state = TransportState::Playing;
    _backendStarted = true;
    lock.unlock();

    if (_backend)
    {
      _backend->start();
    }
  }

  void PlaybackEngine::stop()
  {
    PLAYBACK_LOG_INFO("Playback stopped");

    if (_backend)
    {
      (*_backend).reset();
      _backend->stop();
      _backend->close();
    }

    _source.store({}, std::memory_order_release);
    auto lock = std::lock_guard<std::mutex>{_stateMutex};
    resetToIdle();
  }

  void PlaybackEngine::seek(std::uint32_t positionMs)
  {
    PLAYBACK_LOG_INFO("Seek requested: {} ms", positionMs);
    auto const source = _source.load(std::memory_order_acquire);

    if (!source)
    {
      return;
    }

    bool wasPaused = false;

    {
      auto lock = std::lock_guard<std::mutex>{_stateMutex};
      wasPaused = (_snapshot.state == TransportState::Paused);
      _snapshot.state = TransportState::Buffering;
      _snapshot.positionMs = positionMs;
      _snapshot.statusText.clear();
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
      auto lock = std::lock_guard<std::mutex>{_stateMutex};
      _snapshot.state = TransportState::Error;
      _snapshot.statusText = seekResult.error().message;
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
      auto lock = std::lock_guard<std::mutex>{_stateMutex};
      resetToIdle();
      return;
    }

    if (wasPaused)
    {
      auto lock = std::lock_guard<std::mutex>{_stateMutex};
      _snapshot.state = TransportState::Paused;
      return;
    }

    {
      auto lock = std::lock_guard<std::mutex>{_stateMutex};
      _snapshot.state = TransportState::Playing;
      _backendStarted = true;
    }

    if (_backend)
    {
      _backend->start();
    }
  }

  PlaybackSnapshot PlaybackEngine::snapshot() const
  {
    auto const source = _source.load(std::memory_order_acquire);
    auto lock = std::lock_guard<std::mutex>{_stateMutex};
    auto snap = _snapshot;
    snap.backend = _backend ? _backend->kind() : BackendKind::None;
    snap.bufferedMs = source ? source->bufferedMs() : 0;
    snap.underrunCount = _underrunCount.load(std::memory_order_relaxed);

    return snap;
  }

  void PlaybackEngine::onBackendError(void* userData, std::string_view message) noexcept
  {
    auto* const self = rs::utility::unsafeDowncast<PlaybackEngine>(userData);
    auto msg = std::string(message);

    if (self->_dispatcher)
    {
      self->_dispatcher->dispatch([self, msg = std::move(msg)]() { self->handleBackendError(msg); });
    }
    else
    {
      self->handleBackendError(msg);
    }
  }

  void PlaybackEngine::handleBackendError(std::string_view message)
  {
    PLAYBACK_LOG_ERROR("Backend error: {}", message);

    // Stop immediately
    stop();

    auto lock = std::lock_guard<std::mutex>{_stateMutex};
    _snapshot.state = TransportState::Error;
    _snapshot.statusText = std::string(message);
  }

  bool PlaybackEngine::negotiateFormat(std::filesystem::path const& path,
                                       DecodedStreamInfo const& info,
                                       std::unique_ptr<IAudioDecoderSession>& decoder,
                                       AudioFormat& backendFormat)
  {
    if (!_backend)
    {
      return true;
    }

    auto const backendKind = _backend->kind();

    if (backendKind == BackendKind::PipeWire)
    {
      backendFormat = info.outputFormat;
      PLAYBACK_LOG_INFO("PipeWire shared mode keeps the stream at {}Hz/{}b/{}ch",
                        backendFormat.sampleRate,
                        static_cast<int>(backendFormat.bitDepth),
                        static_cast<int>(backendFormat.channels));
      return true;
    }

    auto const plan = FormatNegotiator::buildPlan(info.sourceFormat, _currentDevice.capabilities);

    if (plan.requiresResample)
    {
      _snapshot.statusText = std::format("{} does not support {} Hz and RockStudio has no resampler yet",
                                         backendDisplayName(backendKind),
                                         info.sourceFormat.sampleRate);
      return false;
    }

    if (plan.requiresChannelRemap)
    {
      _snapshot.statusText = std::format("{} does not support {} channels and RockStudio has no channel remapper yet",
                                         backendDisplayName(backendKind),
                                         static_cast<int>(info.sourceFormat.channels));
      return false;
    }

    PLAYBACK_LOG_INFO("Negotiated Plan: decoder={}b/{}bits, device={}Hz/{}b, reason: {}",
                      static_cast<int>(plan.decoderOutputFormat.bitDepth),
                      static_cast<int>(plan.decoderOutputFormat.validBits),
                      plan.deviceFormat.sampleRate,
                      static_cast<int>(plan.deviceFormat.bitDepth),
                      plan.reason);

    // Re-open decoder if negotiated format differs
    if (!(plan.decoderOutputFormat == info.sourceFormat))
    {
      decoder->close();
      decoder = createAudioDecoderSession(path, plan.decoderOutputFormat);

      if (!decoder)
      {
        _snapshot.statusText = "Failed to re-open decoder with negotiated format";
        return false;
      }

      if (auto const reOpenResult = decoder->open(path); !reOpenResult)
      {
        _snapshot.statusText = reOpenResult.error().message;
        return false;
      }
    }

    backendFormat = plan.deviceFormat;
    return true;
  }

  std::shared_ptr<IPcmSource> PlaybackEngine::createPcmSource(std::unique_ptr<IAudioDecoderSession> decoder,
                                                              DecodedStreamInfo const& info)
  {
    if (shouldUseMemoryPcmSource(info))
    {
      auto memorySource = std::make_shared<rs::audio::MemoryPcmSource>(std::move(decoder), info);

      if (auto const initResult = memorySource->initialize(); !initResult)
      {
        _snapshot.statusText = initResult.error().message;
        return nullptr;
      }

      return memorySource;
    }

    auto streamingSource = std::make_shared<rs::audio::StreamingPcmSource>(
      std::move(decoder),
      info,
      [this](rs::Error const& err)
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
      _snapshot.statusText = initResult.error().message;
      return nullptr;
    }

    return streamingSource;
  }

  bool PlaybackEngine::openTrack(TrackPlaybackDescriptor const& descriptor,
                                 std::shared_ptr<rs::audio::IPcmSource>& source,
                                 AudioFormat& backendFormat)
  {
    auto outputFormat = AudioFormat{};
    outputFormat.sampleRate = 0; // Use native
    outputFormat.channels = 0;   // Use native
    outputFormat.bitDepth = 0;   // Use native
    outputFormat.isFloat = false;
    outputFormat.isInterleaved = true;

    auto decoder = createAudioDecoderSession(descriptor.filePath, outputFormat);

    if (decoder == nullptr)
    {
      _snapshot.statusText = "No audio decoder backend is available";
      return false;
    }

    if (auto const openResult = decoder->open(descriptor.filePath); !openResult)
    {
      _snapshot.statusText = openResult.error().message;
      return false;
    }

    auto info = decoder->streamInfo();

    if (info.outputFormat.sampleRate == 0 || info.outputFormat.channels == 0 || info.outputFormat.bitDepth == 0)
    {
      _snapshot.statusText = "Decoder did not return a valid output format";
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

    _snapshot.durationMs = info.durationMs;
    _snapshot.positionMs = 0;
    _snapshot.graph = {};

    // Add initial Source (Decoder) Node
    _routeSnapshot.graph.nodes.clear();
    _routeSnapshot.graph.nodes.push_back({.id = "rs-decoder",
                                          .type = AudioNodeType::Decoder,
                                          .name = "File Decoder",
                                          .format = info.sourceFormat,
                                          .isLossySource = info.isLossy,
                                          .objectPath = ""});

    // Add Engine Node
    _routeSnapshot.graph.nodes.push_back(rs::audio::AudioNode{.id = "rs-engine",
                                                              .type = rs::audio::AudioNodeType::Engine,
                                                              .name = "RockStudio Engine",
                                                              .format = info.outputFormat,
                                                              .volumeNotUnity = false,
                                                              .isMuted = false,
                                                              .isLossySource = false});

    _routeSnapshot.graph.links.clear();
    _routeSnapshot.graph.links.push_back(
      rs::audio::AudioLink{.sourceId = "rs-decoder", .destId = "rs-engine", .isActive = true});

    _snapshot.graph = _routeSnapshot.graph;

    if (_onRouteChanged)
    {
      auto const snap = _routeSnapshot;

      if (_dispatcher)
      {
        _dispatcher->dispatch([this, snap]() { _onRouteChanged(snap); });
      }
      else
      {
        _onRouteChanged(snap);
      }
    }

    return true;
  }

  std::size_t PlaybackEngine::onReadPcm(void* userData, std::span<std::byte> output) noexcept
  {
    auto* const self = rs::utility::unsafeDowncast<PlaybackEngine>(userData);
    auto const source = self->_source.load(std::memory_order_acquire);

    return source ? source->read(output) : 0;
  }

  bool PlaybackEngine::isSourceDrained(void* userData) noexcept
  {
    auto* const self = rs::utility::unsafeDowncast<PlaybackEngine>(userData);
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

  void PlaybackEngine::onUnderrun(void* userData) noexcept
  {
    auto* const self = rs::utility::unsafeDowncast<PlaybackEngine>(userData);
    ++self->_underrunCount;
  }

  void PlaybackEngine::onPositionAdvanced(void* userData, std::uint32_t frames) noexcept
  {
    auto* const self = rs::utility::unsafeDowncast<PlaybackEngine>(userData);
    auto lock = std::unique_lock<std::mutex>{self->_stateMutex, std::try_to_lock};

    if (!lock.owns_lock())
    {
      return;
    }

    // Use Engine node format for position calculation
    for (auto const& node : self->_snapshot.graph.nodes)
    {
      if (node.type == rs::audio::AudioNodeType::Engine && node.format && node.format->sampleRate > 0)
      {
        auto const ms = (static_cast<std::uint64_t>(frames) * 1000) / node.format->sampleRate;
        self->_snapshot.positionMs += static_cast<std::uint32_t>(ms);
        break;
      }
    }
  }

  void PlaybackEngine::onDrainComplete(void* userData) noexcept
  {
    auto* const self = rs::utility::unsafeDowncast<PlaybackEngine>(userData);

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

  void PlaybackEngine::handleDrainComplete()
  {
    _source.store({}, std::memory_order_release);

    std::function<void()> cb;

    {
      auto lock = std::lock_guard<std::mutex>{_stateMutex};
      resetToIdle();
      cb = _onTrackEnded;
    }

    // Callback OUTSIDE lock — allows play() to re-acquire _stateMutex
    if (cb)
    {
      cb();
    }
  }

  void PlaybackEngine::onRouteReady(void* userData, std::string_view routeAnchor) noexcept
  {
    auto* const self = rs::utility::unsafeDowncast<PlaybackEngine>(userData);
    auto anchor = std::string(routeAnchor);

    if (self->_dispatcher)
    {
      self->_dispatcher->dispatch([self, anchorCopy = std::move(anchor)]() { self->handleRouteReady(anchorCopy); });
    }
    else
    {
      self->handleRouteReady(anchor);
    }
  }

  void PlaybackEngine::handleRouteReady(std::string_view routeAnchor)
  {
    auto lock = std::lock_guard<std::mutex>{_stateMutex};
    _routeSnapshot.anchor = BackendRouteAnchor{
      .backend = _backend ? _backend->kind() : rs::audio::BackendKind::None, .id = std::string(routeAnchor)};

    if (_onRouteChanged)
    {
      auto const snap = _routeSnapshot;

      if (_dispatcher)
      {
        _dispatcher->dispatch([this, snap]() { _onRouteChanged(snap); });
      }
      else
      {
        _onRouteChanged(snap);
      }
    }
  }

  void PlaybackEngine::handleSourceError(rs::Error const& error)
  {
    {
      auto lock = std::lock_guard<std::mutex>{_stateMutex};

      if (_snapshot.state == TransportState::Idle)
      {
        return;
      }

      _backendStarted = false;
      _playbackDrainPending = false;
      _snapshot.state = TransportState::Error;
      _snapshot.statusText = error.message.empty() ? "PCM source failed" : error.message;
    }

    // Backend call OUTSIDE lock to avoid holding _stateMutex during backend operations
    if (_backend)
    {
      _backend->stop();
    }
  }

  void PlaybackEngine::onFormatChanged(void* userData, AudioFormat const& format) noexcept
  {
    auto* const self = rs::utility::unsafeDowncast<PlaybackEngine>(userData);

    if (self->_dispatcher)
    {
      self->_dispatcher->dispatch([self, format]() { self->handleFormatChanged(format); });
    }
    else
    {
      self->handleFormatChanged(format);
    }
  }

  void PlaybackEngine::handleFormatChanged(AudioFormat const& format)
  {
    auto lock = std::lock_guard<std::mutex>{_stateMutex};

    // Update engine node in the route snapshot
    for (auto& node : _routeSnapshot.graph.nodes)
    {
      if (node.id == "rs-engine")
      {
        node.format = format;
        break;
      }
    }

    // Update engine node in the public snapshot
    for (auto& node : _snapshot.graph.nodes)
    {
      if (node.id == "rs-engine")
      {
        node.format = format;
        break;
      }
    }

    if (_onRouteChanged)
    {
      auto const snap = _routeSnapshot;

      if (_dispatcher)
      {
        _dispatcher->dispatch([this, snap]() { _onRouteChanged(snap); });
      }
      else
      {
        _onRouteChanged(snap);
      }
    }
  }

} // namespace rs::audio
