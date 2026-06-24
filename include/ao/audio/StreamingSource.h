// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/audio/DecoderTypes.h>
#include <ao/audio/IDecoderSession.h>
#include <ao/audio/ISource.h>
#include <ao/audio/PcmRingBuffer.h>

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
  class StreamingSource final : public ISource
  {
  public:
    enum class DecodeBlockStatus : std::uint8_t
    {
      Decoded,
      Stopped
    };

    StreamingSource(std::unique_ptr<IDecoderSession> decoderPtr,
                    DecodedStreamInfo streamInfo,
                    std::function<void(Error const&)> onError,
                    std::chrono::milliseconds prerollDuration,
                    std::chrono::milliseconds decodeHighWatermarkThreshold);
    StreamingSource(StreamingSource const&) = delete;
    StreamingSource& operator=(StreamingSource const&) = delete;
    StreamingSource(StreamingSource&&) = delete;
    StreamingSource& operator=(StreamingSource&&) = delete;

    ~StreamingSource() override;

    Result<> initialize();
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

    std::unique_ptr<IDecoderSession> _decoderPtr;
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
    std::chrono::milliseconds _decodeHighWatermarkThreshold{0};
  };
} // namespace ao::audio
