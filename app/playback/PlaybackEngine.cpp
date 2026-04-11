// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "PlaybackEngine.h"

#include <chrono>
#include <iostream>

namespace app::playback
{

  PlaybackEngine::PlaybackEngine(std::unique_ptr<IAudioBackend> backend)
    : _backend(std::move(backend)), _ringBuffer()
  {
  }

  PlaybackEngine::~PlaybackEngine()
  {
    stopDecodeThread();
  }

  void PlaybackEngine::play(TrackPlaybackDescriptor descriptor)
  {
    std::lock_guard<std::mutex> lock(_stateMutex);

    // Stop any existing playback
    stopDecodeThread();

    // Open the track
    _currentTrack = descriptor;
    openTrack(descriptor);

    // Start backend
    if (_backend)
    {
      AudioRenderCallbacks callbacks;
      callbacks.userData = this;
      callbacks.readPcm = &PlaybackEngine::onReadPcm;
      callbacks.onUnderrun = &PlaybackEngine::onUnderrun;
      callbacks.onPositionAdvanced = &PlaybackEngine::onPositionAdvanced;

      auto info = _decoder->streamInfo();
      _backend->open(info.outputFormat, callbacks);
      _backend->start();
    }

    // Start decoding thread
    _decodeThread = std::jthread([this](std::stop_token token) { decodeLoop(token); });

    _state = TransportState::Playing;
    _snapshot.state = TransportState::Playing;
    _snapshot.trackTitle = descriptor.title;
    _snapshot.trackArtist = descriptor.artist;
    _snapshot.durationMs = descriptor.durationMs;
    std::cerr << "[DEBUG] PlaybackEngine::play: done" << std::endl;
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
    std::lock_guard<std::mutex> lock(_stateMutex);
    stopDecodeThread();
    _ringBuffer.clear();
    _decoder.reset();
    _currentTrack.reset();
    _state = TransportState::Idle;
    _snapshot = {};
    _snapshot.state = TransportState::Idle;
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
      _ringBuffer.clear();
      _snapshot.positionMs = positionMs;
    }
  }

  PlaybackSnapshot PlaybackEngine::snapshot() const
  {
    std::lock_guard<std::mutex> lock(_stateMutex);
    auto snap = _snapshot;
    return snap;
  }

  void PlaybackEngine::openTrack(TrackPlaybackDescriptor descriptor)
  {
    StreamFormat outputFormat;
    outputFormat.sampleRate = descriptor.sampleRateHint > 0 ? descriptor.sampleRateHint : 44100;
    outputFormat.channels = descriptor.channelsHint > 0 ? descriptor.channelsHint : 2;
    outputFormat.bitDepth = descriptor.bitDepthHint > 0 ? descriptor.bitDepthHint : 16;
    outputFormat.isInterleaved = true;

    _decoder.emplace(outputFormat);
    _decoder->open(descriptor.filePath);

    auto info = _decoder->streamInfo();
    _snapshot.durationMs = info.durationMs;
    _snapshot.positionMs = 0;
    _snapshot.activeFormat = info.outputFormat;
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
        break;
      }

      if (block->endOfStream)
      {
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
    std::lock_guard<std::mutex> lock(self->_stateMutex);
    if (self->_snapshot.activeFormat && self->_snapshot.activeFormat->sampleRate > 0)
    {
      auto const ms = (static_cast<std::uint64_t>(frames) * 1000) / self->_snapshot.activeFormat->sampleRate;
      self->_snapshot.positionMs += static_cast<std::uint32_t>(ms);
    }
  }

} // namespace app::playback