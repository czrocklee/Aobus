// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "PcmFrameRing.h"
#include "PcmSource.h"
#include <ao/Error.h>
#include <ao/audio/DecodedStreamInfo.h>
#include <ao/audio/DecoderSession.h>
#include <ao/audio/PcmBlock.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <thread>

namespace ao::audio
{
  class StreamingSource final : public PcmSource
  {
  public:
    using OnSourceError = std::function<void(Error const&, std::stop_token)>;
    using OnReady = std::function<void(std::stop_token)>;

    enum class DecodeBlockStatus : std::uint8_t
    {
      Decoded,
      Stopped
    };

    StreamingSource(std::unique_ptr<DecoderSession> decoderPtr,
                    DecodedStreamInfo streamInfo,
                    std::chrono::milliseconds prerollDuration,
                    std::chrono::milliseconds decodeHighWatermarkThreshold);
    StreamingSource(StreamingSource const&) = delete;
    StreamingSource& operator=(StreamingSource const&) = delete;
    StreamingSource(StreamingSource&&) = delete;
    StreamingSource& operator=(StreamingSource&&) = delete;

    ~StreamingSource() override;

    /// Normally prerolls synchronously. An extended seek is reserved for the
    /// decode worker at activation. A failed preparation leaves the source unusable.
    Result<> prepare(std::chrono::milliseconds initialOffset = {});

    /// Install error and ready callbacks exactly once after successful prepare().
    /// Unless already at EOF, start the worker for any pending seek/preroll, then steady-state decoding.
    void activate(OnSourceError onError, OnReady onReady = {});

    // Requires successful prepare() and activate(). The caller quiesces render
    // and query consumers. This method separately stops and joins the
    // source-owned decode producer before resetting PCM.
    Result<> seek(std::chrono::milliseconds offset) noexcept override;

    std::size_t read(std::span<std::byte> output) noexcept override;
    bool isDrained() const noexcept override;
    bool isPreparingSeek() const noexcept override;
    std::chrono::milliseconds bufferedDuration() const noexcept override;

  private:
    enum class Phase : std::uint8_t
    {
      Fresh,
      Prepared,
      Active,
      PreparationFailed
    };

    enum class OnRingFull : std::uint8_t
    {
      RetainRemainder,
      WaitForSpace
    };

    void positionAndPreroll(std::optional<std::chrono::milliseconds> optOffset, std::stop_token const& seekToken);
    void startDecodeThread();
    void stopDecodeThread();
    void decodeLoop(std::stop_token const& seekToken);
    void fillUntil(std::chrono::milliseconds targetBufferedThreshold, std::stop_token const& seekToken);
    DecodeBlockStatus decodeNextBlock(std::stop_token const& seekToken, OnRingFull onFull);
    // Advances bytes in place to the unwritten remainder so retries resume without duplicating PCM.
    bool tryWriteBlock(std::span<std::byte const>& bytes, std::stop_token const& seekToken, OnRingFull onFull);

    std::unique_ptr<DecoderSession> _decoderPtr;
    OnSourceError _onError;
    OnReady _onReady;
    std::atomic<bool> _preparingSeek = false;
    PcmFrameRing _frameRing;
    std::jthread _decodeThread;
    std::stop_source _seekStopSource;
    std::atomic<bool> _decoderReachedEof = false;
    std::uint64_t _bytesPerSecond = 0;
    std::chrono::milliseconds _prerollDuration{0};
    std::size_t _decodeHighWatermarkByteCount = 0;
    Phase _phase = Phase::Fresh;
    std::optional<std::chrono::milliseconds> _optWorkerSeek;

    // Producer-confined. prepare()/seek() run before the decode worker, and
    // seek stops and joins that worker before resetting these values.
    std::size_t _previousBlockByteCount = 0;
    // Borrows decoder storage until fully written. No subsequent decoder read
    // may invalidate it; seek retires it before mutating the decoder.
    std::optional<PcmBlock> _optPendingBlock;
  };
} // namespace ao::audio
