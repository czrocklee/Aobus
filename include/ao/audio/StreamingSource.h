// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/audio/DecodedStreamInfo.h>
#include <ao/audio/DecoderSession.h>
#include <ao/audio/PcmRingBuffer.h>
#include <ao/audio/PcmSource.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <thread>

namespace ao::audio
{
  class StreamingSource final : public PcmSource
  {
  public:
    enum class DecodeBlockStatus : std::uint8_t
    {
      Decoded,
      Stopped
    };

    StreamingSource(std::unique_ptr<DecoderSession> decoderPtr,
                    DecodedStreamInfo streamInfo,
                    std::function<void(Error const&)> onError,
                    std::chrono::milliseconds prerollDuration,
                    std::chrono::milliseconds decodeHighWatermarkThreshold);
    StreamingSource(StreamingSource const&) = delete;
    StreamingSource& operator=(StreamingSource const&) = delete;
    StreamingSource(StreamingSource&&) = delete;
    StreamingSource& operator=(StreamingSource&&) = delete;

    ~StreamingSource() override;

    /// Fills the initial PCM buffer without installing callbacks or starting
    /// the source-owned decode thread. The source may be moved between owners
    /// only by moving its owning pointer after this call returns.
    Result<> prepare();

    /// Installs the source-error callback and starts steady-state decoding.
    /// Must be called exactly once after successful prepare().
    Result<> activate(std::function<void(Error const&)> onError);

    /// Compatibility helper that performs prepare() followed by activate().
    Result<> initialize();

    // Requires successful prepare() and activate(). The caller quiesces render
    // and query consumers. This method separately stops and joins the
    // source-owned decode producer before resetting PCM.
    Result<> seek(std::chrono::milliseconds offset) noexcept override;

    std::size_t read(std::span<std::byte> output) noexcept override;
    bool isDrained() const noexcept override;
    std::chrono::milliseconds bufferedDuration() const noexcept override;

  private:
    void startDecodeThread();
    void stopDecodeThread();
    void decodeLoop(std::stop_token const& threadStopToken);
    void fillUntil(std::chrono::milliseconds targetBufferedThreshold, std::stop_token const& seekToken);
    DecodeBlockStatus decodeNextBlock(std::stop_token const& seekToken, std::stop_token const* threadStopToken);
    bool writeBlock(std::span<std::byte const> bytes,
                    std::stop_token const& seekToken,
                    std::stop_token const* threadStopToken);

    std::unique_ptr<DecoderSession> _decoderPtr;
    DecodedStreamInfo _streamInfo;
    std::function<void(Error const&)> _onError;
    PcmRingBuffer _ringBuffer;
    std::jthread _decodeThread;
    mutable std::mutex _decoderMutex;
    std::stop_source _seekStopSource;
    std::atomic<bool> _decoderReachedEof = false;
    std::atomic<bool> _failed = false;
    mutable std::mutex _errorMutex;
    std::optional<Error> _optLastError;
    std::uint64_t _bytesPerSecond = 0;
    std::chrono::milliseconds _prerollDuration{0};
    std::size_t _decodeHighWatermarkByteCount = 0;
    bool _prepared = false;
    bool _activated = false;

    // Producer-confined. initialize()/seek() run before the decode worker, and
    // seek stops and joins that worker before resetting this value.
    std::size_t _previousBlockByteCount = 0;
  };
} // namespace ao::audio
