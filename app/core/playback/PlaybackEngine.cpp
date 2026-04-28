// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "core/playback/PlaybackEngine.h"
#include "core/Log.h"

#include "core/playback/FormatNegotiator.h"
#include "core/decoder/AudioDecoderFactory.h"
#include "core/decoder/IAudioDecoderSession.h"
#include "core/source/MemoryPcmSource.h"
#include "core/source/StreamingPcmSource.h"

#include <algorithm>
#include <format>
#include <ranges>
#include <limits>
#include <set>
#include <sstream>

namespace app::core::playback
{

  namespace
  {
    constexpr std::uint32_t kPrerollTargetMs = 200;
    constexpr std::uint32_t kDecodeHighWatermarkMs = 750;
    constexpr std::uint64_t kMemoryPcmSourceBudgetBytes = 64ULL * 1024ULL * 1024ULL;

    std::uint64_t bytesPerSecond(AudioFormat const& format) noexcept
    {
      if (format.sampleRate == 0 || format.channels == 0 || format.bitDepth == 0)
      {
        return 0;
      }

      auto const bytesPerSample = (format.bitDepth == 24U) ? 3U : (format.bitDepth > 16U) ? 4U : 2U;
      return static_cast<std::uint64_t>(format.sampleRate) * format.channels * bytesPerSample;
    }

    std::uint64_t estimatedDecodedBytes(app::core::decoder::DecodedStreamInfo const& info) noexcept
    {
      auto const rate = bytesPerSecond(info.outputFormat);
      if (rate == 0 || info.durationMs == 0) return 0;
      return (static_cast<std::uint64_t>(info.durationMs) * rate) / 1000U;
    }

    bool shouldUseMemoryPcmSource(app::core::decoder::DecodedStreamInfo const& info) noexcept
    {
      auto const decodedBytes = estimatedDecodedBytes(info);
      return decodedBytes > 0 && decodedBytes <= kMemoryPcmSourceBudgetBytes;
    }
  } // namespace
  using namespace app::core::backend;
  using namespace app::core::decoder;

  PlaybackEngine::PlaybackEngine(std::unique_ptr<backend::IAudioBackend> backend,
                                 backend::AudioDevice const& device,
                                 std::shared_ptr<IMainThreadDispatcher> dispatcher)
    : _backend{std::move(backend)}, _dispatcher{std::move(dispatcher)}, _currentDevice{device}
  {
    _snapshot.backend = _backend ? _backend->kind() : BackendKind::None;
  }

  PlaybackEngine::~PlaybackEngine()
  {
    stop();
  }

  void PlaybackEngine::setBackend(std::unique_ptr<backend::IAudioBackend> backend, backend::AudioDevice const& device)
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
      return State{
        .track = _currentTrack, .positionMs = _snapshot.positionMs, .wasPlaying = (_snapshot.state == TransportState::Playing)};
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
      auto snap = _routeSnapshot;
      if (_dispatcher) _dispatcher->dispatch([this, snap]() { _onRouteChanged(snap); });
      else _onRouteChanged(snap);
    }
  }


  void PlaybackEngine::play(TrackPlaybackDescriptor descriptor)
  {
    PLAYBACK_LOG_INFO(
      "Play requested: {} - {} [{}]", descriptor.artist, descriptor.title, descriptor.filePath.string());

    if (_backend)
    {
      _backend->open(AudioFormat{}, {});
      _backend->stop();
      _backend->close();
    }

    _source.store({}, std::memory_order_release);

    auto callbacks = backend::AudioRenderCallbacks{};
    callbacks.userData = this;
    callbacks.readPcm = &PlaybackEngine::onReadPcm;
    callbacks.isSourceDrained = &PlaybackEngine::isSourceDrained;
    callbacks.onUnderrun = &PlaybackEngine::onUnderrun;
    callbacks.onPositionAdvanced = &PlaybackEngine::onPositionAdvanced;
    callbacks.onDrainComplete = &PlaybackEngine::onDrainComplete;
    callbacks.onRouteReady = &PlaybackEngine::onRouteReady;
    callbacks.onBackendError = &PlaybackEngine::onBackendError;

    auto source = std::shared_ptr<source::IPcmSource>{};
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

    if (_backend && !_backend->open(backendFormat, callbacks))
    {
      _source.store({}, std::memory_order_release);
      auto lock = std::lock_guard<std::mutex>{_stateMutex};
      _currentTrack.reset();
      _snapshot.state = TransportState::Error;
      _snapshot.statusText = std::string(_backend->lastError());
      return;
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

    if (_backend) _backend->start();
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
    if (shouldPause && _backend) _backend->pause();
  }

  void PlaybackEngine::resume()
  {
    auto source = _source.load(std::memory_order_acquire);
    auto lock = std::unique_lock<std::mutex>{_stateMutex};
    if (_snapshot.state != TransportState::Paused) return;

    PLAYBACK_LOG_INFO("Playback resumed");
    if (_backendStarted)
    {
      _snapshot.state = TransportState::Playing;
      lock.unlock();
      if (_backend) _backend->resume();
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
    if (_backend) _backend->start();
  }

  void PlaybackEngine::stop()
  {
    PLAYBACK_LOG_INFO("Playback stopped");
    if (_backend)
    {
      _backend->open(AudioFormat{}, {});
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
    auto source = _source.load(std::memory_order_acquire);
    if (!source) return;

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

    if (!source->seek(positionMs))
    {
      auto lock = std::lock_guard<std::mutex>{_stateMutex};
      _snapshot.state = TransportState::Error;
      _snapshot.statusText = source->lastError();
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

    if (_backend) _backend->start();
  }

  PlaybackSnapshot PlaybackEngine::snapshot() const
  {
    auto source = _source.load(std::memory_order_acquire);
    auto lock = std::lock_guard<std::mutex>{_stateMutex};
    auto snap = _snapshot;
    snap.backend = _backend ? _backend->kind() : BackendKind::None;
    snap.bufferedMs = source ? source->bufferedMs() : 0;
    snap.underrunCount = _underrunCount.load(std::memory_order_relaxed);

    return snap;
  }

  void PlaybackEngine::onBackendError(void* userData, std::string_view message) noexcept
  {
    auto* self = static_cast<PlaybackEngine*>(userData);
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

  bool PlaybackEngine::openTrack(TrackPlaybackDescriptor descriptor,
                                 std::shared_ptr<source::IPcmSource>& source,
                                 AudioFormat& backendFormat)
  {
    auto outputFormat = AudioFormat{};
    outputFormat.sampleRate = 0; // Use native
    outputFormat.channels = 0;   // Use native
    outputFormat.bitDepth = 0;   // Use native
    outputFormat.isFloat = false;
    outputFormat.isInterleaved = true;

    auto decoder = createAudioDecoderSession(descriptor.filePath, outputFormat);
    if (!decoder)
    {
      _snapshot.statusText = "No audio decoder backend is available";
      return false;
    }
    if (!decoder->open(descriptor.filePath))
    {
      _snapshot.statusText = std::string(decoder->lastError());
      return false;
    }

    auto info = decoder->streamInfo();
    if (info.outputFormat.sampleRate == 0 || info.outputFormat.channels == 0 || info.outputFormat.bitDepth == 0)
    {
      _snapshot.statusText = "Decoder did not return a valid output format";
      return false;
    }

    // --- Format Negotiation ---
    if (_backend)
    {
      auto const caps = _currentDevice.capabilities;
      auto const plan = FormatNegotiator::buildPlan(info.sourceFormat, caps);
      
      PLAYBACK_LOG_INFO("Negotiated Plan: decoder={}b/{}bits, device={}Hz/{}b, reason: {}", 
                        (int)plan.decoderOutputFormat.bitDepth, (int)plan.decoderOutputFormat.validBits,
                        plan.deviceFormat.sampleRate, (int)plan.deviceFormat.bitDepth,
                        plan.reason);
      
      // Re-open decoder with negotiated format if it differs from source
      if (!(plan.decoderOutputFormat == info.sourceFormat))
      {
        decoder->close();
        decoder = createAudioDecoderSession(descriptor.filePath, plan.decoderOutputFormat);
        if (!decoder || !decoder->open(descriptor.filePath))
        {
          _snapshot.statusText = "Failed to re-open decoder with negotiated format";
          return false;
        }
        info = decoder->streamInfo();
      }
      backendFormat = plan.deviceFormat;
    }
    else
    {
      backendFormat = info.outputFormat;
    }

    if (shouldUseMemoryPcmSource(info))
    {
      auto memorySource = std::make_shared<source::MemoryPcmSource>(std::move(decoder), info);
      if (!memorySource->initialize())
      {
        _snapshot.statusText = memorySource->lastError();
        return false;
      }
      source = std::move(memorySource);
    }
    else
    {
      source::PcmSourceCallbacks sourceCallbacks;
      sourceCallbacks.userData = this;
      sourceCallbacks.onError = &PlaybackEngine::onSourceError;
      auto streamingSource = std::make_shared<source::StreamingPcmSource>(
        std::move(decoder), info, sourceCallbacks, kPrerollTargetMs, kDecodeHighWatermarkMs);
      if (!streamingSource->initialize())
      {
        _snapshot.statusText = streamingSource->lastError();
        return false;
      }
      source = std::move(streamingSource);
    }

    _snapshot.durationMs = info.durationMs;
    _snapshot.positionMs = 0;
    _snapshot.graph = {};

    // Add initial Source (Decoder) Node
    _snapshot.graph.nodes.push_back({.id = "rs-decoder",
                                     .type = AudioNodeType::Decoder,
                                     .name = "File Decoder",
                                     .format = info.sourceFormat,
                                     .isLossySource = info.isLossy,
                                     .objectPath = ""});

    // Add Engine Node
    _routeSnapshot.graph.nodes.push_back(backend::AudioNode{
      .id = "rs-engine",
      .type = backend::AudioNodeType::Engine,
      .name = "RockStudio Engine",
      .format = info.outputFormat,
      .volumeNotUnity = false,
      .isMuted = false,
      .isLossySource = false
    });
    
    _routeSnapshot.graph.links.push_back(backend::AudioLink{
      .sourceId = "rs-decoder",
      .destId = "rs-engine",
      .isActive = true
    });

    _snapshot.graph = _routeSnapshot.graph;
    if (_onRouteChanged)
    {
      auto snap = _routeSnapshot;
      if (_dispatcher) _dispatcher->dispatch([this, snap]() { _onRouteChanged(snap); });
      else _onRouteChanged(snap);
    }

    return true;
  }

  std::size_t PlaybackEngine::onReadPcm(void* userData, std::span<std::byte> output) noexcept
  {
    auto* self = static_cast<PlaybackEngine*>(userData);
    auto source = self->_source.load(std::memory_order_acquire);
    return source ? source->read(output) : 0;
  }

  bool PlaybackEngine::isSourceDrained(void* userData) noexcept
  {
    auto* self = static_cast<PlaybackEngine*>(userData);
    auto source = self->_source.load(std::memory_order_acquire);
    if (!source) return true;
    auto const drained = source->isDrained();
    if (drained) self->_playbackDrainPending = true;
    return drained;
  }

  void PlaybackEngine::onUnderrun(void* userData) noexcept
  {
    auto* self = static_cast<PlaybackEngine*>(userData);
    ++self->_underrunCount;
  }

  void PlaybackEngine::onPositionAdvanced(void* userData, std::uint32_t frames) noexcept
  {
    auto* self = static_cast<PlaybackEngine*>(userData);
    auto lock = std::unique_lock<std::mutex>{self->_stateMutex, std::try_to_lock};
    if (!lock.owns_lock()) return;

    // Use Engine node format for position calculation
    for (auto const& node : self->_snapshot.graph.nodes)
    {
      if (node.type == backend::AudioNodeType::Engine && node.format && node.format->sampleRate > 0)
      {
        auto const ms = (static_cast<std::uint64_t>(frames) * 1000) / node.format->sampleRate;
        self->_snapshot.positionMs += static_cast<std::uint32_t>(ms);
        break;
      }
    }
  }

  void PlaybackEngine::onDrainComplete(void* userData) noexcept
  {
    auto* self = static_cast<PlaybackEngine*>(userData);
    if (!self->_playbackDrainPending.exchange(false, std::memory_order_relaxed)) return;

    if (self->_dispatcher) self->_dispatcher->dispatch([self]() { self->handleDrainComplete(); });
    else self->handleDrainComplete();
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
    if (cb) cb();
  }

  void PlaybackEngine::onRouteReady(void* userData, std::string_view routeAnchor) noexcept
  {
    auto* self = static_cast<PlaybackEngine*>(userData);
    auto anchor = std::string(routeAnchor);
    if (self->_dispatcher)
    {
      self->_dispatcher->dispatch([self, a = std::move(anchor)]() { self->handleRouteReady(a); });
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
      .backend = _backend ? _backend->kind() : backend::BackendKind::None,
      .id = std::string(routeAnchor)
    };
    if (_onRouteChanged)
    {
      auto snap = _routeSnapshot;
      if (_dispatcher) _dispatcher->dispatch([this, snap]() { _onRouteChanged(snap); });
      else _onRouteChanged(snap);
    }
  }

  void PlaybackEngine::onSourceError(void* userData) noexcept
  {
    auto* self = static_cast<PlaybackEngine*>(userData);
    auto source = self->_source.load(std::memory_order_acquire);
    auto const errorText = source ? source->lastError() : std::string{};
    
    if (self->_dispatcher)
    {
      self->_dispatcher->dispatch([self, errorText]() { self->handleSourceError(errorText); });
    }
    else
    {
      self->handleSourceError(errorText);
    }
  }

  void PlaybackEngine::handleSourceError(std::string const& message)
  {
    {
      auto lock = std::lock_guard<std::mutex>{_stateMutex};
      if (_snapshot.state == TransportState::Idle) return;
      _backendStarted = false;
      _playbackDrainPending = false;
      _snapshot.state = TransportState::Error;
      _snapshot.statusText = message.empty() ? "PCM source failed" : message;
    }

    // Backend call OUTSIDE lock to avoid holding _stateMutex during backend operations
    if (_backend)
    {
      _backend->stop();
    }
  }

} // namespace app::core::playback
