// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "StreamingSource.h"

#include "detail/DecoderError.h"
#include "detail/StreamingBufferPolicy.h"
#include <ao/Contract.h>
#include <ao/Error.h>
#include <ao/audio/DecodedStreamInfo.h>
#include <ao/audio/DecoderSession.h>
#include <ao/audio/PcmBlock.h>
#include <ao/audio/PcmFormat.h>
#include <ao/utility/ThreadName.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <memory>
#include <optional>
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

  StreamingSource::StreamingSource(std::unique_ptr<DecoderSession> decoderPtr,
                                   DecodedStreamInfo streamInfo,
                                   std::chrono::milliseconds prerollDuration,
                                   std::chrono::milliseconds decodeHighWatermarkThreshold)
    : _decoderPtr{std::move(decoderPtr)}
    , _frameRing{frameBytes(streamInfo.outputFormat)}
    , _bytesPerSecond{bytesPerSecond(streamInfo.outputFormat)}
    , _prerollDuration{prerollDuration}
    , _decodeHighWatermarkByteCount{
        detail::bufferByteCountForDuration(_bytesPerSecond, decodeHighWatermarkThreshold, _frameRing.byteCapacity())}
  {
  }

  StreamingSource::~StreamingSource()
  {
    stopDecodeThread();
  }

  Result<> StreamingSource::prepare(std::chrono::milliseconds initialOffset)
  {
    AO_EXPECTS(_phase == Phase::Fresh, "Streaming source preparation requires a fresh inactive source");

    auto const seekToken = _seekStopSource.get_token();

    try
    {
      positionAndPreroll(initialOffset.count() > 0 ? std::optional{initialOffset} : std::nullopt, seekToken);

      _phase = Phase::Prepared;
      return {};
    }
    catch (detail::DecoderException const& ex)
    {
      _phase = Phase::PreparationFailed;
      return std::unexpected{ex.error()};
    }
  }

  void StreamingSource::activate(OnSourceError onError, OnReady onReady)
  {
    AO_EXPECTS(_phase == Phase::Prepared, "Streaming source activation requires one prepared source");

    _onError = std::move(onError);
    _onReady = std::move(onReady);
    _phase = Phase::Active;

    if (!_decoderReachedEof.load(std::memory_order_relaxed))
    {
      startDecodeThread();
    }
  }

  std::size_t StreamingSource::read(std::span<std::byte> output) noexcept
  {
    return isPreparingSeek() ? 0 : _frameRing.read(output);
  }

  bool StreamingSource::isDrained() const noexcept
  {
    return !isPreparingSeek() && _decoderReachedEof.load(std::memory_order_relaxed) &&
           _frameRing.readableByteCount() == 0;
  }

  bool StreamingSource::isPreparingSeek() const noexcept
  {
    return _preparingSeek.load(std::memory_order_acquire);
  }

  std::chrono::milliseconds StreamingSource::bufferedDuration() const noexcept
  {
    return calculateBufferedDuration(_frameRing.readableByteCount(), _bytesPerSecond);
  }

  // Result error materialization and decoder-thread startup may allocate; the
  // streaming source intentionally fails fast if that escapes its noexcept contract.
  Result<> StreamingSource::seek(std::chrono::milliseconds offset) noexcept
  {
    AO_EXPECTS(_phase == Phase::Active, "Streaming source seek requires an activated source");

    stopDecodeThread();
    _seekStopSource = std::stop_source{};
    auto const seekToken = _seekStopSource.get_token();

    _decoderReachedEof = false;
    _frameRing.clear();
    _previousBlockByteCount = 0;
    _optPendingBlock.reset();

    try
    {
      positionAndPreroll(offset, seekToken);

      if (!_decoderReachedEof.load(std::memory_order_relaxed))
      {
        startDecodeThread();
      }

      // All failures from the newly started worker use the attempt-tagged
      // callback; its scheduling must not change this command's result.
      return {};
    }
    catch (detail::DecoderException const& ex)
    {
      if (_onError)
      {
        _onError(ex.error(), seekToken);
      }

      return std::unexpected{ex.error()};
    }
  }

  void StreamingSource::positionAndPreroll(std::optional<std::chrono::milliseconds> optOffset,
                                           std::stop_token const& seekToken)
  {
    // Called only with the producer quiescent. Never expose old buffered PCM
    // while a superseding seek is waiting for the worker.
    _optWorkerSeek.reset();
    _preparingSeek.store(false, std::memory_order_relaxed);

    if (optOffset)
    {
      if (_decoderPtr->needsSeekWorker(*optOffset))
      {
        _optWorkerSeek = optOffset;
        _preparingSeek.store(true, std::memory_order_release);
        return;
      }

      if (auto const res = _decoderPtr->seek(*optOffset); !res)
      {
        detail::throwDecoderError(res.error());
      }
    }

    fillUntil(_prerollDuration, seekToken);
  }

  void StreamingSource::startDecodeThread()
  {
    AO_INVARIANT(!_decodeThread.joinable(), "Streaming source already has a decode worker");
    _decodeThread = std::jthread{[this, token = _seekStopSource.get_token()]
                                 {
                                   setCurrentThreadName("StreamingSource-Decode");

                                   try
                                   {
                                     decodeLoop(token);
                                   }
                                   catch (...)
                                   {
                                     AO_FATAL_EXCEPTION(std::current_exception(), "streaming-source decode thread");
                                   }
                                 }};
  }

  void StreamingSource::stopDecodeThread()
  {
    // Invalidate queued callbacks even when the attempt has no live worker.
    _seekStopSource.request_stop();

    if (_decodeThread.joinable())
    {
      _decodeThread.join();
    }
  }

  void StreamingSource::decodeLoop(std::stop_token const& seekToken)
  {
    try
    {
      if (auto const optOffset = std::exchange(_optWorkerSeek, std::nullopt); optOffset)
      {
        // No previous PCM can be consumed here. A cancelled worker is joined
        // before another seek, and a failing attempt never opens the render gate.
        auto const res = _decoderPtr->seekAfterDiscard(*optOffset, seekToken);

        if (seekToken.stop_requested())
        {
          return;
        }

        if (!res)
        {
          detail::throwDecoderError(res.error());
        }

        // No render consumer before readiness. Retain an oversized block's
        // unwritten tail exactly as in synchronous preroll, and observe stop
        // between reads so a replacement seek can promptly join this worker.
        fillUntil(_prerollDuration, seekToken);

        if (seekToken.stop_requested())
        {
          return;
        }

        _preparingSeek.store(false, std::memory_order_release);

        if (_onReady)
        {
          _onReady(seekToken);
        }
      }

      while (!_decoderReachedEof.load(std::memory_order_relaxed) && !seekToken.stop_requested())
      {
        // An already decoded block must finish before admission of another
        // read, even when its original size exceeds the remaining capacity.
        if (!_optPendingBlock && !detail::canDecode(_decodeHighWatermarkByteCount,
                                                    _frameRing.readableByteCount(),
                                                    _frameRing.writableByteCount(),
                                                    _previousBlockByteCount))
        {
          std::this_thread::sleep_for(kDecodeBackoffInterval);
          continue;
        }

        auto const status = decodeNextBlock(seekToken, OnRingFull::WaitForSpace);

        if (status == StreamingSource::DecodeBlockStatus::Stopped)
        {
          break;
        }
      }
    }
    catch (detail::DecoderException const& ex)
    {
      if (!seekToken.stop_requested() && _onError)
      {
        _onError(ex.error(), seekToken);
      }
    }
  }

  void StreamingSource::fillUntil(std::chrono::milliseconds targetBufferedThreshold, std::stop_token const& seekToken)
  {
    auto const targetByteCount =
      detail::bufferByteCountForDuration(_bytesPerSecond, targetBufferedThreshold, _frameRing.byteCapacity());

    while (!_decoderReachedEof.load(std::memory_order_relaxed) && !seekToken.stop_requested() &&
           detail::canDecode(
             targetByteCount, _frameRing.readableByteCount(), _frameRing.writableByteCount(), _previousBlockByteCount))
    {
      if (auto const status = decodeNextBlock(seekToken, OnRingFull::RetainRemainder);
          status == StreamingSource::DecodeBlockStatus::Stopped)
      {
        break;
      }
    }
  }

  StreamingSource::DecodeBlockStatus StreamingSource::decodeNextBlock(std::stop_token const& seekToken,
                                                                      OnRingFull onFull)
  {
    if (seekToken.stop_requested())
    {
      return DecodeBlockStatus::Stopped;
    }

    if (!_optPendingBlock)
    {
      auto const blockRes = _decoderPtr->readNextBlock();

      if (!blockRes)
      {
        detail::throwDecoderError(blockRes.error());
      }

      if (blockRes->bytes.size() > _frameRing.byteCapacity())
      {
        detail::throwDecoderError(Error::Code::DecodeFailed, "Decoded PCM block exceeds streaming buffer capacity");
      }

      if (blockRes->bytes.size() % _frameRing.frameByteCount() != 0)
      {
        detail::throwDecoderError(Error::Code::DecodeFailed, "Decoded PCM block is not a whole number of frames");
      }

      if (!blockRes->bytes.empty())
      {
        _previousBlockByteCount = blockRes->bytes.size();
      }

      _optPendingBlock = *blockRes;
    }

    auto& pendingBlock = *_optPendingBlock;

    if (!tryWriteBlock(pendingBlock.bytes, seekToken, onFull))
    {
      return DecodeBlockStatus::Stopped;
    }

    bool const endOfStream = pendingBlock.endOfStream;
    _optPendingBlock.reset();

    if (endOfStream)
    {
      _decoderReachedEof = true;
      return DecodeBlockStatus::Stopped;
    }

    return DecodeBlockStatus::Decoded;
  }

  bool StreamingSource::tryWriteBlock(std::span<std::byte const>& bytes,
                                      std::stop_token const& seekToken,
                                      OnRingFull onFull)
  {
    while (!bytes.empty() && !seekToken.stop_requested())
    {
      auto const written = _frameRing.write(bytes);
      bytes = bytes.subspan(written);

      if (!bytes.empty())
      {
        // Preroll has no render consumer to make space, even when run on the worker.
        if (onFull == OnRingFull::RetainRemainder)
        {
          return false;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds{2});
      }
    }

    return bytes.empty();
  }
} // namespace ao::audio
