// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <ao/audio/StreamingSource.h>
#include <ao/utility/Log.h>
#include <ao/utility/ThreadUtils.h>

#include <chrono>

namespace ao::audio
{
  namespace
  {
    constexpr std::uint8_t kBytesPer24BitSample = 3;

    std::uint64_t bytesPerSecond(Format const& format) noexcept
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

    std::uint32_t bufferedDurationMs(std::size_t byteCount, std::uint64_t bytesPerSecondValue) noexcept
    {
      if (bytesPerSecondValue == 0)
      {
        return 0;
      }

      return static_cast<std::uint32_t>((static_cast<std::uint64_t>(byteCount) * 1000U) / bytesPerSecondValue);
    }
  } // namespace

  StreamingSource::StreamingSource(std::unique_ptr<IDecoderSession> decoder,
                                   DecodedStreamInfo streamInfo,
                                   std::function<void(ao::Error const&)> onError,
                                   std::uint32_t prerollTargetMs,
                                   std::uint32_t decodeHighWatermarkMs)
    : _decoder{std::move(decoder)}
    , _streamInfo{streamInfo}
    , _onError{std::move(onError)}
    , _bytesPerSecond{bytesPerSecond(streamInfo.outputFormat)}
    , _prerollTargetMs{prerollTargetMs}
    , _decodeHighWatermarkMs{decodeHighWatermarkMs}
  {
  }

  StreamingSource::~StreamingSource()
  {
    stopDecodeThread();
  }

  ao::Result<> StreamingSource::initialize()
  {
    auto const seekToken = _seekStopSource.get_token();

    if (auto const fillResult = fillUntil(_prerollTargetMs, seekToken); !fillResult)
    {
      return fillResult;
    }

    if (!_decoderReachedEof.load(std::memory_order_relaxed))
    {
      startDecodeThread();
    }

    if (_failed.load(std::memory_order_relaxed))
    {
      return std::unexpected(
        ao::Error{.code = ao::Error::Code::Generic, .message = "Streaming source failed during initialization"});
    }

    return {};
  }

  std::size_t StreamingSource::read(std::span<std::byte> output) noexcept
  {
    return _ringBuffer.read(output);
  }

  bool StreamingSource::isDrained() const noexcept
  {
    return _decoderReachedEof.load(std::memory_order_relaxed) && _ringBuffer.size() == 0;
  }

  std::uint32_t StreamingSource::bufferedMs() const noexcept
  {
    return bufferedDurationMs(_ringBuffer.size(), _bytesPerSecond);
  }

  ao::Result<> StreamingSource::seek(std::uint32_t positionMs)
  {
    stopDecodeThread();

    _seekStopSource.request_stop();
    _seekStopSource = std::stop_source{};
    auto const seekToken = _seekStopSource.get_token();

    _failed = false;
    _decoderReachedEof = false;
    _ringBuffer.clear();

    {
      auto lock = std::lock_guard<std::mutex>{_decoderMutex};

      auto const seekResult = _decoder->seek(positionMs);

      if (!seekResult)
      {
        if (!_failed.exchange(true, std::memory_order_relaxed))
        {
          if (_onError)
          {
            _onError(seekResult.error());
          }
        }

        return seekResult;
      }
    }

    if (auto const fillResult = fillUntil(_prerollTargetMs, seekToken); !fillResult)
    {
      return fillResult;
    }

    if (!_decoderReachedEof.load(std::memory_order_relaxed))
    {
      startDecodeThread();
    }

    if (_failed.load(std::memory_order_relaxed))
    {
      return std::unexpected(
        ao::Error{.code = ao::Error::Code::Generic, .message = "Streaming source is in failed state"});
    }

    return {};
  }

  void StreamingSource::startDecodeThread()
  {
    stopDecodeThread();

    _decodeThread = std::jthread(
      [this](std::stop_token const& token)
      {
        ao::setCurrentThreadName("StreamingSource-Decode");
        decodeLoop(token);
      });
  }

  void StreamingSource::stopDecodeThread()
  {
    if (_decodeThread.joinable())
    {
      _decodeThread.request_stop();
      _decodeThread.join();
    }
  }

  void StreamingSource::decodeLoop(std::stop_token const& threadStopToken)
  {
    auto const seekToken = _seekStopSource.get_token();

    while (!threadStopToken.stop_requested() && !_failed.load(std::memory_order_relaxed) &&
           !_decoderReachedEof.load(std::memory_order_relaxed) && !seekToken.stop_requested())
    {
      if (bufferedMs() >= _decodeHighWatermarkMs)
      {
        std::this_thread::sleep_for(std::chrono::milliseconds(5)); // NOLINT(readability-magic-numbers)
        continue;
      }

      auto const result = decodeNextBlock(seekToken, &threadStopToken);

      if (!result)
      {
        if (!_failed.exchange(true, std::memory_order_relaxed))
        {
          if (_onError)
          {
            _onError(result.error());
          }
        }

        break;
      }

      if (!*result)
      {
        break;
      }
    }
  }

  ao::Result<> StreamingSource::fillUntil(std::uint32_t targetBufferedMs, std::stop_token const& seekToken)
  {
    while (!_failed.load(std::memory_order_relaxed) && !_decoderReachedEof.load(std::memory_order_relaxed) &&
           !seekToken.stop_requested() && bufferedMs() < targetBufferedMs)
    {
      auto const result = decodeNextBlock(seekToken, nullptr);

      if (!result)
      {
        if (!_failed.exchange(true, std::memory_order_relaxed))
        {
          if (_onError)
          {
            _onError(result.error());
          }
        }

        return std::unexpected(result.error());
      }

      if (!*result)
      {
        break;
      }
    }

    if (_failed.load(std::memory_order_relaxed))
    {
      return std::unexpected(
        ao::Error{.code = ao::Error::Code::Generic, .message = "Streaming source is in failed state"});
    }

    return {};
  }

  ao::Result<bool> StreamingSource::decodeNextBlock(std::stop_token const& seekToken,
                                                    std::stop_token const* threadStopToken)
  {
    if (seekToken.stop_requested())
    {
      return false;
    }

    auto block = PcmBlock{};

    {
      auto lock = std::lock_guard<std::mutex>{_decoderMutex};

      if (seekToken.stop_requested())
      {
        return false;
      }

      auto const blockResult = _decoder->readNextBlock();

      if (!blockResult)
      {
        return std::unexpected(blockResult.error());
      }

      block = *blockResult;
    }

    if (block.endOfStream)
    {
      _decoderReachedEof = true;
      return false;
    }

    return writeBlock(std::span<std::byte const>(block.bytes.data(), block.bytes.size()), seekToken, threadStopToken);
  }

  bool StreamingSource::writeBlock(std::span<std::byte const> bytes,
                                   std::stop_token const& seekToken,
                                   std::stop_token const* threadStopToken)
  {
    auto const stopRequested = [&]()
    { return seekToken.stop_requested() || (threadStopToken && threadStopToken->stop_requested()); };

    auto const* current = bytes.data();
    auto remaining = bytes.size();

    while (remaining > 0 && !stopRequested())
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
} // namespace ao::audio
