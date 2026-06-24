// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/Error.h>
#include <ao/audio/DecoderTypes.h>
#include <ao/audio/Format.h>
#include <ao/audio/IDecoderSession.h>
#include <ao/audio/StreamingSource.h>
#include <ao/audio/detail/DecoderError.h>
#include <ao/utility/ThreadUtils.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <stop_token>
#include <thread>
#include <utility>

namespace ao::audio
{
  namespace
  {
    constexpr auto kDecodeBackoffInterval = std::chrono::milliseconds{5};

    std::chrono::milliseconds calculateBufferedDuration(std::size_t byteCount,
                                                        std::uint64_t bytesPerSecondValue) noexcept
    {
      if (bytesPerSecondValue == 0)
      {
        return std::chrono::milliseconds{0};
      }

      return std::chrono::milliseconds{(static_cast<std::uint64_t>(byteCount) * 1000U) / bytesPerSecondValue};
    }
  } // namespace

  StreamingSource::StreamingSource(std::unique_ptr<IDecoderSession> decoderPtr,
                                   DecodedStreamInfo streamInfo,
                                   std::function<void(Error const&)> onError,
                                   std::chrono::milliseconds prerollDuration,
                                   std::chrono::milliseconds decodeHighWatermarkThreshold)
    : _decoderPtr{std::move(decoderPtr)}
    , _streamInfo{streamInfo}
    , _onError{std::move(onError)}
    , _bytesPerSecond{bytesPerSecond(streamInfo.outputFormat)}
    , _prerollDuration{prerollDuration}
    , _decodeHighWatermarkThreshold{decodeHighWatermarkThreshold}
  {
  }

  StreamingSource::~StreamingSource()
  {
    stopDecodeThread();
  }

  Result<> StreamingSource::initialize()
  {
    auto const seekToken = _seekStopSource.get_token();

    try
    {
      fillUntil(_prerollDuration, seekToken);

      if (!_decoderReachedEof.load(std::memory_order_relaxed))
      {
        startDecodeThread();
      }

      if (_failed.load(std::memory_order_relaxed))
      {
        auto const lock = std::scoped_lock{_errorMutex};
        return std::unexpected(_optLastError.value_or(
          Error{.code = Error::Code::InvalidState, .message = "Streaming source failed during initialization"}));
      }

      return {};
    }
    catch (detail::DecoderException const& ex)
    {
      bool const failedExchanged = !_failed.exchange(true, std::memory_order_relaxed);

      {
        auto const lock = std::scoped_lock{_errorMutex};
        _optLastError = ex.error();
      }

      if (failedExchanged && _onError)
      {
        _onError(ex.error());
      }

      return std::unexpected{ex.error()};
    }
  }

  std::size_t StreamingSource::read(std::span<std::byte> output) noexcept
  {
    return _ringBuffer.read(output);
  }

  bool StreamingSource::isDrained() const noexcept
  {
    return _decoderReachedEof.load(std::memory_order_relaxed) && _ringBuffer.size() == 0;
  }

  std::chrono::milliseconds StreamingSource::bufferedDuration() const noexcept
  {
    return calculateBufferedDuration(_ringBuffer.size(), _bytesPerSecond);
  }

  Result<> StreamingSource::seek(std::chrono::milliseconds offset) noexcept
  {
    stopDecodeThread();

    _seekStopSource.request_stop();
    _seekStopSource = std::stop_source{};
    auto const seekToken = _seekStopSource.get_token();

    {
      auto const lock = std::scoped_lock{_errorMutex};
      _optLastError.reset();
      _failed.store(false, std::memory_order_relaxed);
    }
    _decoderReachedEof = false;
    _ringBuffer.clear();

    try
    {
      {
        auto lock = std::scoped_lock{_decoderMutex};

        if (auto const res = _decoderPtr->seek(offset); !res)
        {
          detail::throwDecoderError(res.error());
        }
      }

      fillUntil(_prerollDuration, seekToken);

      if (!_decoderReachedEof.load(std::memory_order_relaxed))
      {
        startDecodeThread();
      }

      if (_failed.load(std::memory_order_relaxed))
      {
        auto const lock = std::scoped_lock{_errorMutex};
        return std::unexpected(_optLastError.value_or(
          Error{.code = Error::Code::InvalidState, .message = "Streaming source is in failed state"}));
      }

      return {};
    }
    catch (detail::DecoderException const& ex)
    {
      bool const failedExchanged = !_failed.exchange(true, std::memory_order_relaxed);

      {
        auto const lock = std::scoped_lock{_errorMutex};
        _optLastError = ex.error();
      }

      if (failedExchanged && _onError)
      {
        _onError(ex.error());
      }

      return std::unexpected{ex.error()};
    }
  }

  void StreamingSource::startDecodeThread()
  {
    stopDecodeThread();

    _decodeThread = std::jthread{[this](std::stop_token const& token)
                                 {
                                   setCurrentThreadName("StreamingSource-Decode");
                                   decodeLoop(token);
                                 }};
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

    try
    {
      while (!threadStopToken.stop_requested() && !_failed.load(std::memory_order_relaxed) &&
             !_decoderReachedEof.load(std::memory_order_relaxed) && !seekToken.stop_requested())
      {
        if (bufferedDuration() >= _decodeHighWatermarkThreshold)
        {
          std::this_thread::sleep_for(kDecodeBackoffInterval);
          continue;
        }

        auto const status = decodeNextBlock(seekToken, &threadStopToken);

        if (status == StreamingSource::DecodeBlockStatus::Stopped)
        {
          break;
        }
      }
    }
    catch (detail::DecoderException const& ex)
    {
      {
        auto const lock = std::scoped_lock{_errorMutex};
        _optLastError = ex.error();
      }

      if (!_failed.exchange(true, std::memory_order_relaxed))
      {
        if (_onError)
        {
          _onError(ex.error());
        }
      }
    }
  }

  void StreamingSource::fillUntil(std::chrono::milliseconds targetBufferedThreshold, std::stop_token const& seekToken)
  {
    while (!_failed.load(std::memory_order_relaxed) && !_decoderReachedEof.load(std::memory_order_relaxed) &&
           !seekToken.stop_requested() && bufferedDuration() < targetBufferedThreshold)
    {
      if (auto const status = decodeNextBlock(seekToken, nullptr);
          status == StreamingSource::DecodeBlockStatus::Stopped)
      {
        break;
      }
    }

    if (_failed.load(std::memory_order_relaxed))
    {
      auto const lock = std::scoped_lock{_errorMutex};
      auto const err = _optLastError.value_or(
        Error{.code = Error::Code::InvalidState, .message = "Streaming source is in failed state"});
      detail::throwDecoderError(err);
    }
  }

  StreamingSource::DecodeBlockStatus StreamingSource::decodeNextBlock(std::stop_token const& seekToken,
                                                                      std::stop_token const* threadStopToken)
  {
    if (seekToken.stop_requested())
    {
      return DecodeBlockStatus::Stopped;
    }

    auto block = PcmBlock{};

    {
      auto lock = std::scoped_lock{_decoderMutex};

      if (seekToken.stop_requested())
      {
        return DecodeBlockStatus::Stopped;
      }

      auto const blockResult = _decoderPtr->readNextBlock();

      if (!blockResult)
      {
        detail::throwDecoderError(blockResult.error());
      }

      block = *blockResult;
    }

    if (block.endOfStream)
    {
      _decoderReachedEof = true;
      return DecodeBlockStatus::Stopped;
    }

    if (!writeBlock(std::span<std::byte const>{block.bytes.data(), block.bytes.size()}, seekToken, threadStopToken))
    {
      return DecodeBlockStatus::Stopped;
    }

    return DecodeBlockStatus::Decoded;
  }

  bool StreamingSource::writeBlock(std::span<std::byte const> bytes,
                                   std::stop_token const& seekToken,
                                   std::stop_token const* threadStopToken)
  {
    auto const stopRequested = [&]
    { return seekToken.stop_requested() || (threadStopToken && threadStopToken->stop_requested()); };

    auto const* current = bytes.data();
    std::size_t remaining = bytes.size();

    while (remaining > 0 && !stopRequested())
    {
      auto const written = _ringBuffer.write(std::span<std::byte const>{current, remaining});
      remaining -= written;
      current += written;

      if (remaining > 0)
      {
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
      }
    }

    return remaining == 0;
  }
} // namespace ao::audio
