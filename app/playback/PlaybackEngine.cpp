// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "PlaybackEngine.h"

#include <chrono>

namespace
{
  constexpr std::uint32_t kPrerollTargetMs = 200;
  constexpr std::uint32_t kDecodeHighWatermarkMs = 750;

  std::uint64_t bytesPerSecond(app::playback::StreamFormat const& format) noexcept
  {
    if (format.sampleRate == 0 || format.channels == 0 || format.bitDepth == 0)
    {
      return 0;
    }

    auto const bytesPerSample = (format.bitDepth == 24U) ? 3U : (format.bitDepth > 16U) ? 4U : 2U;
    return static_cast<std::uint64_t>(format.sampleRate) * format.channels * bytesPerSample;
  }

  std::uint32_t bufferedDurationMs(std::size_t byteCount, std::uint64_t bytesPerSecondValue) noexcept
  {
    if (bytesPerSecondValue == 0)
    {
      return 0;
    }

    return static_cast<std::uint32_t>((static_cast<std::uint64_t>(byteCount) * 1000U) / bytesPerSecondValue);
  }
} // namespace

namespace app::playback
{

  PlaybackEngine::PlaybackEngine(std::unique_ptr<IAudioBackend> backend)
    : _backend(std::move(backend)), _ringBuffer()
  {
    _snapshot.backend = _backend ? _backend->kind() : BackendKind::None;
  }

  PlaybackEngine::~PlaybackEngine()
  {
    stop();
  }

  void PlaybackEngine::play(TrackPlaybackDescriptor descriptor)
  {
    stopDecodeThread();

    if (_backend)
    {
      _backend->stop();
      _backend->close();
    }

    AudioRenderCallbacks callbacks;
    callbacks.userData = this;
    callbacks.readPcm = &PlaybackEngine::onReadPcm;
    callbacks.isSourceDrained = &PlaybackEngine::isSourceDrained;
    callbacks.onUnderrun = &PlaybackEngine::onUnderrun;
    callbacks.onPositionAdvanced = &PlaybackEngine::onPositionAdvanced;
    callbacks.onDrainComplete = &PlaybackEngine::onDrainComplete;

    StreamFormat backendFormat;

    {
      std::lock_guard<std::mutex> lock(_stateMutex);

      _ringBuffer.clear();
      _bufferedMs = 0;
      _underrunCount = 0;
      _bytesPerSecond = 0;
      _backendStarted = false;
      _decoderReachedEof = false;
      _playbackDrainPending = false;

      _snapshot = {};
      _snapshot.backend = _backend ? _backend->kind() : BackendKind::None;
      _snapshot.state = TransportState::Opening;
      _snapshot.trackTitle = descriptor.title;
      _snapshot.trackArtist = descriptor.artist;

      _currentTrack = descriptor;
      if (!openTrack(descriptor))
      {
        _state = TransportState::Error;
        _snapshot.state = TransportState::Error;
        _currentTrack.reset();
        return;
      }

      backendFormat = _decoder->streamInfo().outputFormat;
      _bytesPerSecond = bytesPerSecond(backendFormat);
      _state = TransportState::Buffering;
      _snapshot.state = TransportState::Buffering;
    }

    if (_backend && !_backend->open(backendFormat, callbacks))
    {
      std::lock_guard<std::mutex> lock(_stateMutex);
      _decoder.reset();
      _currentTrack.reset();
      _state = TransportState::Error;
      _snapshot.state = TransportState::Error;
      _snapshot.statusText = std::string(_backend->lastError());
      return;
    }

    _decodeThread = std::jthread([this](std::stop_token token) { decodeLoop(token); });
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
    std::lock_guard<std::mutex> lock(_stateMutex);
    if (_state == TransportState::Paused)
    {
      if (_backendStarted)
      {
        _state = TransportState::Playing;
        _snapshot.state = TransportState::Playing;
        _backend->resume();
      }
      else
      {
        _state = TransportState::Buffering;
        _snapshot.state = TransportState::Buffering;
      }
    }
  }

  void PlaybackEngine::stop()
  {
    stopDecodeThread();

    std::lock_guard<std::mutex> lock(_stateMutex);
    _ringBuffer.clear();
    _decoder.reset();
    _currentTrack.reset();
    _bufferedMs = 0;
    _bytesPerSecond = 0;
    _backendStarted = false;
    _decoderReachedEof = false;
    _playbackDrainPending = false;
    _state = TransportState::Idle;
    _snapshot = {};
    _snapshot.backend = _backend ? _backend->kind() : BackendKind::None;
    if (_backend)
    {
      _backend->stop();
      _backend->close();
    }
  }

  void PlaybackEngine::seek(std::uint32_t positionMs)
  {
    std::lock_guard<std::mutex> lock(_stateMutex);
    std::lock_guard<std::mutex> decoderLock(_decoderMutex);
    if (_decoder)
    {
      auto const wasPaused = (_state == TransportState::Paused);

      _decoder->seek(positionMs);
      if (_backend)
      {
        _backend->stop();
        _backend->flush();
      }

      _backendStarted = false;
      _decoderReachedEof = false;
      _playbackDrainPending = false;
      _ringBuffer.clear();
      _bufferedMs = 0;
      _state = wasPaused ? TransportState::Paused : TransportState::Buffering;
      _snapshot.state = wasPaused ? TransportState::Paused : TransportState::Buffering;
      _snapshot.positionMs = positionMs;
    }
  }

  PlaybackSnapshot PlaybackEngine::snapshot() const
  {
    std::lock_guard<std::mutex> lock(_stateMutex);
    auto snap = _snapshot;
    snap.backend = _backend ? _backend->kind() : BackendKind::None;
    snap.bufferedMs = bufferedDurationMs(_ringBuffer.size(), _bytesPerSecond.load(std::memory_order_relaxed));
    snap.underrunCount = _underrunCount.load(std::memory_order_relaxed);
    return snap;
  }

  bool PlaybackEngine::openTrack(TrackPlaybackDescriptor descriptor)
  {
    StreamFormat outputFormat;
    outputFormat.sampleRate = descriptor.sampleRateHint;
    outputFormat.channels = descriptor.channelsHint;
    outputFormat.bitDepth = descriptor.bitDepthHint;
    outputFormat.isFloat = false;
    outputFormat.isInterleaved = true;

    _decoder.emplace(outputFormat);
    if (!_decoder->open(descriptor.filePath))
    {
      _snapshot.statusText = std::string(_decoder->lastError());
      _snapshot.activeFormat.reset();
      _decoder.reset();
      return false;
    }

    auto info = _decoder->streamInfo();
    if (info.outputFormat.sampleRate == 0 || info.outputFormat.channels == 0 || info.outputFormat.bitDepth == 0)
    {
      _snapshot.statusText = "Decoder did not return a valid output format";
      _snapshot.activeFormat.reset();
      _decoder.reset();
      return false;
    }

    _snapshot.durationMs = info.durationMs;
    _snapshot.positionMs = 0;
    _snapshot.activeFormat = info.outputFormat;
    return true;
  }

  void PlaybackEngine::stopDecodeThread()
  {
    if (_decodeThread.joinable())
    {
      _decodeThread.request_stop();
      _decodeThread.join();
    }
  }

  void PlaybackEngine::decodeLoop(std::stop_token stopToken)
  {
    while (!stopToken.stop_requested())
    {
      auto const state = _state.load(std::memory_order_relaxed);
      if (state != TransportState::Playing && state != TransportState::Buffering)
      {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        continue;
      }

      auto const bufferedMs = bufferedDurationMs(_ringBuffer.size(), _bytesPerSecond.load(std::memory_order_relaxed));
      _bufferedMs.store(bufferedMs, std::memory_order_relaxed);

      bool shouldStartBackend = false;
      {
        std::lock_guard<std::mutex> lock(_stateMutex);
        if (_state == TransportState::Buffering && !_backendStarted && bufferedMs >= kPrerollTargetMs)
        {
          _state = TransportState::Playing;
          _snapshot.state = TransportState::Playing;
          _backendStarted = true;
          shouldStartBackend = true;
        }
      }

      if (shouldStartBackend && _backend)
      {
        _backend->start();
        continue;
      }

      if (bufferedMs >= kDecodeHighWatermarkMs)
      {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        continue;
      }

      std::optional<PcmBlock> block;
      {
        std::lock_guard<std::mutex> lock(_decoderMutex);
        if (!_decoder)
        {
          break;
        }
        block = _decoder->readNextBlock();
      }

      if (!block)
      {
        {
          std::lock_guard<std::mutex> lock(_stateMutex);
          _state = TransportState::Error;
          _snapshot.state = TransportState::Error;
          if (_decoder)
          {
            _snapshot.statusText = std::string(_decoder->lastError());
          }
        }

        if (_backend)
        {
          _backend->stop();
        }

        break;
      }

      if (block->endOfStream)
      {
        _decoderReachedEof = true;

        auto const finalBufferedMs = bufferedDurationMs(_ringBuffer.size(), _bytesPerSecond.load(std::memory_order_relaxed));
        _bufferedMs.store(finalBufferedMs, std::memory_order_relaxed);

        bool shouldStartAtEof = false;
        bool shouldCompleteImmediately = false;
        {
          std::lock_guard<std::mutex> stateLock(_stateMutex);
          if (!_backendStarted && finalBufferedMs > 0 && _state == TransportState::Buffering)
          {
            _state = TransportState::Playing;
            _snapshot.state = TransportState::Playing;
            _backendStarted = true;
            shouldStartAtEof = true;
          }
          else if (!_backendStarted && finalBufferedMs == 0)
          {
            _decoder.reset();
            _currentTrack.reset();
            _bytesPerSecond = 0;
            _state = TransportState::Idle;
            _snapshot = {};
            _snapshot.backend = _backend ? _backend->kind() : BackendKind::None;
            shouldCompleteImmediately = true;
          }
          else
          {
            _playbackDrainPending = true;
          }
        }

        if (shouldStartAtEof && _backend)
        {
          _playbackDrainPending = true;
          _backend->start();
        }

        if (shouldCompleteImmediately && _backend)
        {
          _backend->stop();
          _backend->close();
        }
        else if (_backend && _backend->kind() == BackendKind::None && _playbackDrainPending)
        {
          _backend->drain();
        }

        break;
      }

      // Write all bytes to ring buffer using loop+sleep approach
      std::span<std::byte const> bytes(block->bytes.data(), block->bytes.size());
      auto toWrite = bytes.size();
      auto* current = bytes.data();

      while (toWrite > 0 && !stopToken.stop_requested())
      {
        auto pushed = _ringBuffer.write(std::span<std::byte const>(current, toWrite));
        toWrite -= pushed;
        current += pushed;

        if (toWrite > 0)
        {
          // Queue full, sleep and retry
          std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
      }

      auto const postWriteBufferedMs = bufferedDurationMs(_ringBuffer.size(), _bytesPerSecond.load(std::memory_order_relaxed));
      _bufferedMs.store(postWriteBufferedMs, std::memory_order_relaxed);
    }
  }

  std::size_t PlaybackEngine::onReadPcm(void* userData, std::span<std::byte> output) noexcept
  {
    auto* self = static_cast<PlaybackEngine*>(userData);
    return self->_ringBuffer.read(output);
  }

  bool PlaybackEngine::isSourceDrained(void* userData) noexcept
  {
    auto* self = static_cast<PlaybackEngine*>(userData);
    return self->_decoderReachedEof.load(std::memory_order_relaxed) && self->_ringBuffer.size() == 0;
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

    std::lock_guard<std::mutex> lock(self->_stateMutex);
    self->_ringBuffer.clear();
    self->_decoder.reset();
    self->_currentTrack.reset();
    self->_bufferedMs = 0;
    self->_bytesPerSecond = 0;
    self->_backendStarted = false;
    self->_decoderReachedEof = false;
    self->_state = TransportState::Idle;
    self->_snapshot = {};
    self->_snapshot.backend = self->_backend ? self->_backend->kind() : BackendKind::None;
  }

} // namespace app::playback
