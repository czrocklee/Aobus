// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "core/source/StreamingPcmSource.h"
#include "core/util/ThreadUtils.h"

#include <chrono>

namespace app::core::source
{
  namespace
  {
    std::uint64_t bytesPerSecond(AudioFormat const& format) noexcept
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

  StreamingPcmSource::StreamingPcmSource(std::unique_ptr<decoder::IAudioDecoderSession> decoder,
                                         decoder::DecodedStreamInfo streamInfo,
                                         PcmSourceCallbacks callbacks,
                                         std::uint32_t prerollTargetMs,
                                         std::uint32_t decodeHighWatermarkMs)
    : _decoder{std::move(decoder)}
    , _streamInfo{streamInfo}
    , _callbacks{callbacks}
    , _bytesPerSecond{bytesPerSecond(streamInfo.outputFormat)}
    , _prerollTargetMs{prerollTargetMs}
    , _decodeHighWatermarkMs{decodeHighWatermarkMs}
  {
  }

  StreamingPcmSource::~StreamingPcmSource()
  {
    stopDecodeThread();
  }

  bool StreamingPcmSource::initialize()
  {
    if (auto const generation = _generation.load(std::memory_order_relaxed); !fillUntil(_prerollTargetMs, generation))
    {
      return false;
    }

    if (!_decoderReachedEof.load(std::memory_order_relaxed))
    {
      startDecodeThread();
    }

    return !_failed.load(std::memory_order_relaxed);
  }

  std::size_t StreamingPcmSource::read(std::span<std::byte> output) noexcept
  {
    return _ringBuffer.read(output);
  }

  bool StreamingPcmSource::isDrained() const noexcept
  {
    return _decoderReachedEof.load(std::memory_order_relaxed) && _ringBuffer.size() == 0;
  }

  std::uint32_t StreamingPcmSource::bufferedMs() const noexcept
  {
    return bufferedDurationMs(_ringBuffer.size(), _bytesPerSecond);
  }

  bool StreamingPcmSource::seek(std::uint32_t positionMs)
  {
    stopDecodeThread();

    auto const generation = _generation.fetch_add(1, std::memory_order_relaxed) + 1;
    clearError();
    _decoderReachedEof = false;
    _ringBuffer.clear();

    {
      auto lock = std::lock_guard<std::mutex>{_decoderMutex};

      if (!_decoder->seek(positionMs))
      {
        fail(std::string(_decoder->lastError()));
        return false;
      }
    }

    if (!fillUntil(_prerollTargetMs, generation))
    {
      return false;
    }

    if (!_decoderReachedEof.load(std::memory_order_relaxed))
    {
      startDecodeThread();
    }

    return !_failed.load(std::memory_order_relaxed);
  }

  std::string StreamingPcmSource::lastError() const
  {
    auto lock = std::lock_guard<std::mutex>{_errorMutex};
    return _lastError;
  }

  void StreamingPcmSource::startDecodeThread()
  {
    stopDecodeThread();
    _decodeThread = std::jthread(
      [this](std::stop_token token)
      {
        app::core::util::setCurrentThreadName("StreamingPcmSource-Decode");
        decodeLoop(token);
      });
  }

  void StreamingPcmSource::stopDecodeThread()
  {
    if (_decodeThread.joinable())
    {
      _decodeThread.request_stop();
      _decodeThread.join();
    }
  }

  void StreamingPcmSource::decodeLoop(std::stop_token stopToken)
  {
    auto const generation = _generation.load(std::memory_order_relaxed);

    while (!stopToken.stop_requested() && !_failed.load(std::memory_order_relaxed) &&
           !_decoderReachedEof.load(std::memory_order_relaxed) &&
           _generation.load(std::memory_order_relaxed) == generation)
    {
      if (bufferedMs() >= _decodeHighWatermarkMs)
      {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        continue;
      }

      if (!decodeNextBlock(generation, &stopToken))
      {
        break;
      }
    }
  }

  bool StreamingPcmSource::fillUntil(std::uint32_t targetBufferedMs, std::uint64_t generation)
  {
    while (!_failed.load(std::memory_order_relaxed) && !_decoderReachedEof.load(std::memory_order_relaxed) &&
           _generation.load(std::memory_order_relaxed) == generation && bufferedMs() < targetBufferedMs)
    {
      if (!decodeNextBlock(generation, nullptr))
      {
        break;
      }
    }

    return !_failed.load(std::memory_order_relaxed);
  }

  bool StreamingPcmSource::decodeNextBlock(std::uint64_t generation, std::stop_token const* stopToken)
  {
    std::optional<decoder::PcmBlock> block;
    std::string errorText;
    {
      auto lock = std::lock_guard<std::mutex>{_decoderMutex};

      if (_generation.load(std::memory_order_relaxed) != generation)
      {
        return false;
      }

      block = _decoder->readNextBlock();

      if (!block)
      {
        errorText = std::string(_decoder->lastError());
      }
    }

    if (!block)
    {
      fail(std::move(errorText));
      return false;
    }

    if (block->endOfStream)
    {
      _decoderReachedEof = true;
      return false;
    }

    return writeBlock(std::span<std::byte const>(block->bytes.data(), block->bytes.size()), generation, stopToken);
  }

  bool StreamingPcmSource::writeBlock(std::span<std::byte const> bytes,
                                      std::uint64_t generation,
                                      std::stop_token const* stopToken)
  {
    auto const stopRequested = [stopToken]() { return stopToken && stopToken->stop_requested(); };

    auto* current = bytes.data();
    auto remaining = bytes.size();

    while (remaining > 0 && !stopRequested() && _generation.load(std::memory_order_relaxed) == generation)
    {
      auto const written = _ringBuffer.write(std::span<std::byte const>(current, remaining));
      remaining -= written;
      current += written;

      if (remaining > 0)
      {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
      }
    }

    return remaining == 0;
  }

  void StreamingPcmSource::fail(std::string message)
  {
    if (_failed.exchange(true, std::memory_order_relaxed))
    {
      return;
    }

    {
      auto lock = std::lock_guard<std::mutex>{_errorMutex};
      _lastError = std::move(message);
    }

    if (_callbacks.onError)
    {
      _callbacks.onError(_callbacks.userData);
    }
  }

  void StreamingPcmSource::clearError()
  {
    _failed = false;
    auto lock = std::lock_guard<std::mutex>{_errorMutex};
    _lastError.clear();
  }

} // namespace app::core::playback
