// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "PlaybackEngine.h"

#include <chrono>

namespace
{
  std::uint32_t bufferedDurationMs(std::size_t byteCount,
                                   std::optional<app::playback::StreamFormat> const& format) noexcept
  {
    if (!format || format->sampleRate == 0 || format->channels == 0 || format->bitDepth == 0)
    {
      return 0;
    }

    auto const bytesPerSample = (format->bitDepth == 24U) ? 3U : (format->bitDepth > 16U) ? 4U : 2U;
    auto const bytesPerSecond =
      static_cast<std::uint64_t>(format->sampleRate) * format->channels * bytesPerSample;
    if (bytesPerSecond == 0)
    {
      return 0;
    }

    return static_cast<std::uint32_t>((static_cast<std::uint64_t>(byteCount) * 1000U) / bytesPerSecond);
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
    stopDecodeThread();
  }

  void PlaybackEngine::play(TrackPlaybackDescriptor descriptor)
  {
    stopDecodeThread();

    if (_backend)
    {
      _backend->stop();
    }

    AudioRenderCallbacks callbacks;
    callbacks.userData = this;
    callbacks.readPcm = &PlaybackEngine::onReadPcm;
    callbacks.onUnderrun = &PlaybackEngine::onUnderrun;
    callbacks.onPositionAdvanced = &PlaybackEngine::onPositionAdvanced;

    StreamFormat backendFormat;

    {
      std::lock_guard<std::mutex> lock(_stateMutex);

      _ringBuffer.clear();
      _bufferedMs = 0;
      _underrunCount = 0;

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
      _state = TransportState::Playing;
      _snapshot.state = TransportState::Playing;
    }

    if (_backend)
    {
      _backend->open(backendFormat, callbacks);
    }

    _decodeThread = std::jthread([this](std::stop_token token) { decodeLoop(token); });

    if (_backend)
    {
      _backend->start();
    }
  }

  void PlaybackEngine::pause()
  {
    std::lock_guard<std::mutex> lock(_stateMutex);
    if (_state == TransportState::Playing)
    {
      _state = TransportState::Paused;
      _snapshot.state = TransportState::Paused;
      if (_backend)
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
      _state = TransportState::Playing;
      _snapshot.state = TransportState::Playing;
      if (_backend)
      {
        _backend->resume();
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
    _state = TransportState::Idle;
    _snapshot = {};
    _snapshot.backend = _backend ? _backend->kind() : BackendKind::None;
    if (_backend)
    {
      _backend->stop();
    }
  }

  void PlaybackEngine::seek(std::uint32_t positionMs)
  {
    std::lock_guard<std::mutex> lock(_stateMutex);
    std::lock_guard<std::mutex> decoderLock(_decoderMutex);
    if (_decoder)
    {
      _decoder->seek(positionMs);
      if (_backend)
      {
        _backend->flush();
      }
      _ringBuffer.clear();
      _bufferedMs = 0;
      _snapshot.positionMs = positionMs;
    }
  }

  PlaybackSnapshot PlaybackEngine::snapshot() const
  {
    std::lock_guard<std::mutex> lock(_stateMutex);
    auto snap = _snapshot;
    snap.backend = _backend ? _backend->kind() : BackendKind::None;
    snap.bufferedMs = bufferedDurationMs(_ringBuffer.size(), snap.activeFormat);
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
      if (_state != TransportState::Playing)
      {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
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
        {
          std::lock_guard<std::mutex> stateLock(_stateMutex);
          std::lock_guard<std::mutex> decoderLock(_decoderMutex);
          _ringBuffer.clear();
          _decoder.reset();
          _currentTrack.reset();
          _state = TransportState::Idle;
          _snapshot = {};
          _snapshot.state = TransportState::Idle;
        }

        if (_backend)
        {
          _backend->stop();
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
    }
  }

  std::size_t PlaybackEngine::onReadPcm(void* userData, std::span<std::byte> output) noexcept
  {
    auto* self = static_cast<PlaybackEngine*>(userData);
    return self->_ringBuffer.read(output);
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

} // namespace app::playback
