// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "PlaybackEngine.h"

#include "MemoryPcmSource.h"
#include "StreamingPcmSource.h"

#include <limits>

namespace
{
  constexpr std::uint32_t kPrerollTargetMs = 200;
  constexpr std::uint32_t kDecodeHighWatermarkMs = 750;
  constexpr std::uint64_t kMemoryPcmSourceBudgetBytes = 64ULL * 1024ULL * 1024ULL;

  bool sameStreamFormat(app::playback::StreamFormat const& lhs, app::playback::StreamFormat const& rhs) noexcept
  {
    return lhs.sampleRate == rhs.sampleRate && lhs.channels == rhs.channels && lhs.bitDepth == rhs.bitDepth &&
           lhs.isFloat == rhs.isFloat && lhs.isInterleaved == rhs.isInterleaved;
  }

  void appendTooltipLine(std::string& tooltip, std::string_view line)
  {
    if (line.empty())
    {
      return;
    }

    if (!tooltip.empty())
    {
      tooltip += '\n';
    }
    tooltip += line;
  }

  void raiseSinkStatus(app::playback::BackendFormatInfo::SinkStatus& current,
                       app::playback::BackendFormatInfo::SinkStatus candidate) noexcept
  {
    if (static_cast<int>(candidate) > static_cast<int>(current))
    {
      current = candidate;
    }
  }

  std::uint64_t bytesPerSecond(app::playback::StreamFormat const& format) noexcept
  {
    if (format.sampleRate == 0 || format.channels == 0 || format.bitDepth == 0)
    {
      return 0;
    }

    auto const bytesPerSample = (format.bitDepth == 24U) ? 3U : (format.bitDepth > 16U) ? 4U : 2U;
    return static_cast<std::uint64_t>(format.sampleRate) * format.channels * bytesPerSample;
  }

  std::uint64_t estimatedDecodedBytes(app::playback::DecodedStreamInfo const& info) noexcept
  {
    auto const rate = bytesPerSecond(info.outputFormat);
    if (rate == 0 || info.durationMs == 0)
    {
      return 0;
    }

    return (static_cast<std::uint64_t>(info.durationMs) * rate) / 1000U;
  }

  bool shouldUseMemoryPcmSource(app::playback::DecodedStreamInfo const& info) noexcept
  {
    auto const decodedBytes = estimatedDecodedBytes(info);
    return decodedBytes > 0 && decodedBytes <= kMemoryPcmSourceBudgetBytes;
  }
} // namespace

namespace app::playback
{

  PlaybackEngine::PlaybackEngine(std::unique_ptr<IAudioBackend> backend)
    : _backend(std::move(backend))
  {
    _snapshot.backend = _backend ? _backend->kind() : BackendKind::None;
  }

  PlaybackEngine::~PlaybackEngine()
  {
    stop();
  }

  void PlaybackEngine::play(TrackPlaybackDescriptor descriptor)
  {
    if (_backend)
    {
      _backend->stop();
      _backend->close();
    }

    _source.store({}, std::memory_order_release);

    AudioRenderCallbacks callbacks;
    callbacks.userData = this;
    callbacks.readPcm = &PlaybackEngine::onReadPcm;
    callbacks.isSourceDrained = &PlaybackEngine::isSourceDrained;
    callbacks.onUnderrun = &PlaybackEngine::onUnderrun;
    callbacks.onPositionAdvanced = &PlaybackEngine::onPositionAdvanced;
    callbacks.onDrainComplete = &PlaybackEngine::onDrainComplete;

    std::shared_ptr<IPcmSource> source;
    StreamFormat backendFormat;

    {
      std::lock_guard<std::mutex> lock(_stateMutex);

      _underrunCount = 0;
      _backendStarted = false;
      _playbackDrainPending = false;

      _snapshot = {};
      _snapshot.backend = _backend ? _backend->kind() : BackendKind::None;
      _snapshot.state = TransportState::Opening;
      _snapshot.trackTitle = descriptor.title;
      _snapshot.trackArtist = descriptor.artist;

      _currentTrack = descriptor;
      if (!openTrack(descriptor, source, backendFormat))
      {
        _state = TransportState::Error;
        _snapshot.state = TransportState::Error;
        _currentTrack.reset();
        return;
      }
      _state = TransportState::Buffering;
      _snapshot.state = TransportState::Buffering;
    }

    _source.store(source, std::memory_order_release);

    if (_backend && !_backend->open(backendFormat, callbacks))
    {
      _source.store({}, std::memory_order_release);
      std::lock_guard<std::mutex> lock(_stateMutex);
      _currentTrack.reset();
      _state = TransportState::Error;
      _snapshot.state = TransportState::Error;
      _snapshot.statusText = std::string(_backend->lastError());
      return;
    }

    if (_backend)
    {
      auto const backendInfo = _backend->formatInfo();
      std::lock_guard<std::mutex> lock(_stateMutex);
      if (backendInfo.streamFormat)
      {
        _snapshot.activeFormat = backendInfo.streamFormat;
      }
      _snapshot.deviceFormat = backendInfo.deviceFormat;
      _snapshot.exclusiveOutput = backendInfo.isExclusive;
      _snapshot.conversionReason = backendInfo.conversionReason;
    }

    auto const bufferedMs = source ? source->bufferedMs() : 0;
    auto const drained = !source || source->isDrained();

    if (drained && bufferedMs == 0)
    {
      if (_backend)
      {
        _backend->stop();
        _backend->close();
      }

      _source.store({}, std::memory_order_release);

      std::lock_guard<std::mutex> lock(_stateMutex);
      _currentTrack.reset();
      _backendStarted = false;
      _playbackDrainPending = false;
      _state = TransportState::Idle;
      _snapshot = {};
      _snapshot.backend = _backend ? _backend->kind() : BackendKind::None;
      return;
    }

    {
      std::lock_guard<std::mutex> lock(_stateMutex);
      _state = TransportState::Playing;
      _snapshot.state = TransportState::Playing;
      _backendStarted = true;
    }

    if (_backend && _backend->kind() == BackendKind::None)
    {
      _playbackDrainPending = true;
      _backend->drain();
      return;
    }

    if (_backend)
    {
      _backend->start();
    }
  }

  void PlaybackEngine::pause()
  {
    std::lock_guard<std::mutex> lock(_stateMutex);
    if (_state == TransportState::Playing || _state == TransportState::Buffering)
    {
      _state = TransportState::Paused;
      _snapshot.state = TransportState::Paused;
      if (_backend && _backendStarted)
      {
        _backend->pause();
      }
    }
  }

  void PlaybackEngine::resume()
  {
    auto source = _source.load(std::memory_order_acquire);

    std::unique_lock<std::mutex> lock(_stateMutex);
    if (_state != TransportState::Paused)
    {
      return;
    }

    if (_backendStarted)
    {
      _state = TransportState::Playing;
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
      _currentTrack.reset();
      _state = TransportState::Idle;
      _snapshot = {};
      _snapshot.backend = _backend ? _backend->kind() : BackendKind::None;
      return;
    }

    _state = TransportState::Playing;
    _snapshot.state = TransportState::Playing;
    _backendStarted = true;
    lock.unlock();

    if (_backend && _backend->kind() == BackendKind::None)
    {
      _playbackDrainPending = true;
      _backend->drain();
      return;
    }

    if (_backend)
    {
      _backend->start();
    }
  }

  void PlaybackEngine::stop()
  {
    if (_backend)
    {
      _backend->stop();
      _backend->close();
    }

    _source.store({}, std::memory_order_release);

    std::lock_guard<std::mutex> lock(_stateMutex);
    _currentTrack.reset();
    _backendStarted = false;
    _playbackDrainPending = false;
    _state = TransportState::Idle;
    _snapshot = {};
    _snapshot.backend = _backend ? _backend->kind() : BackendKind::None;
  }

  void PlaybackEngine::seek(std::uint32_t positionMs)
  {
    auto source = _source.load(std::memory_order_acquire);
    if (!source)
    {
      return;
    }

    bool wasPaused = false;
    {
      std::lock_guard<std::mutex> lock(_stateMutex);
      wasPaused = (_state == TransportState::Paused);
      _state = TransportState::Buffering;
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
      std::lock_guard<std::mutex> lock(_stateMutex);
      _state = TransportState::Error;
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

      std::lock_guard<std::mutex> lock(_stateMutex);
      _currentTrack.reset();
      _state = TransportState::Idle;
      _snapshot = {};
      _snapshot.backend = _backend ? _backend->kind() : BackendKind::None;
      return;
    }

    if (wasPaused)
    {
      std::lock_guard<std::mutex> lock(_stateMutex);
      _state = TransportState::Paused;
      _snapshot.state = TransportState::Paused;
      return;
    }

    {
      std::lock_guard<std::mutex> lock(_stateMutex);
      _state = TransportState::Playing;
      _snapshot.state = TransportState::Playing;
      _backendStarted = true;
    }

    if (_backend && _backend->kind() == BackendKind::None)
    {
      _playbackDrainPending = true;
      _backend->drain();
      return;
    }

    if (_backend)
    {
      _backend->start();
    }
  }

  PlaybackSnapshot PlaybackEngine::snapshot() const
  {
    auto source = _source.load(std::memory_order_acquire);
    auto const backendInfo = _backend ? _backend->formatInfo() : BackendFormatInfo{};
    std::lock_guard<std::mutex> lock(_stateMutex);
    auto snap = _snapshot;
    snap.backend = _backend ? _backend->kind() : BackendKind::None;
    snap.bufferedMs = source ? source->bufferedMs() : 0;
    snap.underrunCount = _underrunCount.load(std::memory_order_relaxed);
    if (backendInfo.streamFormat)
    {
      snap.activeFormat = backendInfo.streamFormat;
    }
    snap.deviceFormat = backendInfo.deviceFormat;
    snap.exclusiveOutput = backendInfo.isExclusive;
    snap.conversionReason = backendInfo.conversionReason;
    snap.sinkName = backendInfo.sinkName;
    snap.sinkStatus = backendInfo.sinkStatus;
    snap.sinkTooltip = backendInfo.sinkTooltip;

    if (snap.sinkStatus != BackendFormatInfo::SinkStatus::None && snap.sourceFormat && snap.activeFormat &&
        !sameStreamFormat(*snap.sourceFormat, *snap.activeFormat))
    {
      raiseSinkStatus(snap.sinkStatus, BackendFormatInfo::SinkStatus::Bad);
      appendTooltipLine(snap.sinkTooltip, "Decoded source format does not match the PipeWire stream format.");
    }

    if (snap.sinkStatus != BackendFormatInfo::SinkStatus::None && snap.sourceFormat && snap.deviceFormat &&
        !sameStreamFormat(*snap.sourceFormat, *snap.deviceFormat))
    {
      raiseSinkStatus(snap.sinkStatus, BackendFormatInfo::SinkStatus::Bad);
      appendTooltipLine(snap.sinkTooltip, "Decoded source format does not match the active sink format.");
    }

    if (snap.sinkStatus != BackendFormatInfo::SinkStatus::None)
    {
      appendTooltipLine(snap.sinkTooltip,
                        "This is a best-effort server/backend inspection and does not verify conversions after the sink node.");
    }
    return snap;
  }

  bool PlaybackEngine::openTrack(TrackPlaybackDescriptor descriptor,
                                 std::shared_ptr<IPcmSource>& source,
                                 StreamFormat& backendFormat)
  {
    StreamFormat outputFormat;
    outputFormat.sampleRate = descriptor.sampleRateHint;
    outputFormat.channels = descriptor.channelsHint;
    outputFormat.bitDepth = descriptor.bitDepthHint;
    outputFormat.isFloat = false;
    outputFormat.isInterleaved = true;

    auto decoder = FfmpegDecoderSession{outputFormat};
    if (!decoder.open(descriptor.filePath))
    {
      _snapshot.statusText = std::string(decoder.lastError());
      _snapshot.activeFormat.reset();
      return false;
    }

    auto const info = decoder.streamInfo();
    if (info.outputFormat.sampleRate == 0 || info.outputFormat.channels == 0 || info.outputFormat.bitDepth == 0)
    {
      _snapshot.statusText = "Decoder did not return a valid output format";
      _snapshot.activeFormat.reset();
      return false;
    }

    if (shouldUseMemoryPcmSource(info))
    {
      auto memorySource = std::make_shared<MemoryPcmSource>(std::move(decoder), info);
      if (!memorySource->initialize())
      {
        _snapshot.statusText = memorySource->lastError();
        _snapshot.activeFormat.reset();
        return false;
      }
      source = std::move(memorySource);
    }
    else
    {
      PcmSourceCallbacks sourceCallbacks;
      sourceCallbacks.userData = this;
      sourceCallbacks.onError = &PlaybackEngine::onSourceError;

      auto streamingSource =
        std::make_shared<StreamingPcmSource>(std::move(decoder),
                                             info,
                                             sourceCallbacks,
                                             kPrerollTargetMs,
                                             kDecodeHighWatermarkMs);
      if (!streamingSource->initialize())
      {
        _snapshot.statusText = streamingSource->lastError();
        _snapshot.activeFormat.reset();
        return false;
      }
      source = std::move(streamingSource);
    }

    _snapshot.durationMs = info.durationMs;
    _snapshot.positionMs = 0;
    _snapshot.sourceFormat = info.sourceFormat;
    _snapshot.activeFormat = info.outputFormat;
    _snapshot.deviceFormat.reset();
    _snapshot.exclusiveOutput = false;
    _snapshot.conversionReason.clear();
    _snapshot.sinkName.clear();
    _snapshot.sinkStatus = BackendFormatInfo::SinkStatus::None;
    _snapshot.sinkTooltip.clear();
    backendFormat = info.outputFormat;
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
    auto* self = static_cast<PlaybackEngine*>(userData);
    ++self->_underrunCount;
  }

  void PlaybackEngine::onPositionAdvanced(void* userData, std::uint32_t frames) noexcept
  {
    auto* self = static_cast<PlaybackEngine*>(userData);
    // Estimate position based on frames consumed
    std::unique_lock<std::mutex> lock(self->_stateMutex, std::try_to_lock);
    if (!lock.owns_lock())
    {
      return;
    }

    if (self->_snapshot.activeFormat && self->_snapshot.activeFormat->sampleRate > 0)
    {
      auto const ms = (static_cast<std::uint64_t>(frames) * 1000) / self->_snapshot.activeFormat->sampleRate;
      self->_snapshot.positionMs += static_cast<std::uint32_t>(ms);
    }
  }

  void PlaybackEngine::onDrainComplete(void* userData) noexcept
  {
    auto* self = static_cast<PlaybackEngine*>(userData);
    if (!self->_playbackDrainPending.exchange(false, std::memory_order_relaxed))
    {
      return;
    }

    self->_source.store({}, std::memory_order_release);

    std::lock_guard<std::mutex> lock(self->_stateMutex);
    self->_currentTrack.reset();
    self->_backendStarted = false;
    self->_state = TransportState::Idle;
    self->_snapshot = {};
    self->_snapshot.backend = self->_backend ? self->_backend->kind() : BackendKind::None;
  }

  void PlaybackEngine::onSourceError(void* userData) noexcept
  {
    auto* self = static_cast<PlaybackEngine*>(userData);
    auto source = self->_source.load(std::memory_order_acquire);
    auto const errorText = source ? source->lastError() : std::string{};

    {
      std::lock_guard<std::mutex> lock(self->_stateMutex);
      if (self->_state == TransportState::Idle)
      {
        return;
      }

      self->_backendStarted = false;
      self->_playbackDrainPending = false;
      self->_state = TransportState::Error;
      self->_snapshot.state = TransportState::Error;
      self->_snapshot.statusText = errorText.empty() ? "PCM source failed" : errorText;
    }

    if (self->_backend)
    {
      self->_backend->stop();
    }
  }

} // namespace app::playback
