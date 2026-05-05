// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/audio/IDecoderSession.h>
#include <ao/audio/ISource.h>
#include <ao/audio/PcmRingBuffer.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>

namespace ao::audio
{
  class StreamingSource final : public ISource
  {
  public:
    StreamingSource(std::unique_ptr<IDecoderSession> decoder,
                    DecodedStreamInfo streamInfo,
                    std::function<void(ao::Error const&)> onError,
                    std::uint32_t prerollTargetMs,
                    std::uint32_t decodeHighWatermarkMs);
    ~StreamingSource() override;

    ao::Result<> initialize();
    ao::Result<> seek(std::uint32_t positionMs) override;

    std::size_t read(std::span<std::byte> output) noexcept override;
    bool isDrained() const noexcept override;
    std::uint32_t bufferedMs() const noexcept override;

  private:
    void startDecodeThread();
    void stopDecodeThread();
    void decodeLoop(std::stop_token const& threadStopToken);
    ao::Result<> fillUntil(std::uint32_t targetBufferedMs, std::stop_token const& seekToken);
    ao::Result<bool> decodeNextBlock(std::stop_token const& seekToken, std::stop_token const* threadStopToken);
    bool writeBlock(std::span<std::byte const> bytes,
                    std::stop_token const& seekToken,
                    std::stop_token const* threadStopToken);

    std::unique_ptr<IDecoderSession> _decoder;
    DecodedStreamInfo _streamInfo;
    std::function<void(ao::Error const&)> _onError;
    PcmRingBuffer _ringBuffer;
    std::jthread _decodeThread;
    mutable std::mutex _decoderMutex;
    std::stop_source _seekStopSource;
    std::atomic<bool> _decoderReachedEof = false;
    std::atomic<bool> _failed = false;
    std::uint64_t _bytesPerSecond = 0;
    std::uint32_t _prerollTargetMs = 0;
    std::uint32_t _decodeHighWatermarkMs = 0;
  };
} // namespace ao::audio
