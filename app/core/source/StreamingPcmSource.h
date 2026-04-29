// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include "core/decoder/IAudioDecoderSession.h"
#include "core/source/IPcmSource.h"
#include "core/source/PcmRingBuffer.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>

namespace app::core::source
{

  class StreamingPcmSource final : public IPcmSource
  {
  public:
    StreamingPcmSource(std::unique_ptr<decoder::IAudioDecoderSession> decoder,
                       decoder::DecodedStreamInfo streamInfo,
                       std::function<void(rs::Error const&)> onError,
                       std::uint32_t prerollTargetMs,
                       std::uint32_t decodeHighWatermarkMs);
    ~StreamingPcmSource() override;

    rs::Result<> initialize();
    rs::Result<> seek(std::uint32_t positionMs) override;

    std::size_t read(std::span<std::byte> output) noexcept override;
    bool isDrained() const noexcept override;
    std::uint32_t bufferedMs() const noexcept override;

  private:
    void startDecodeThread();
    void stopDecodeThread();
    void decodeLoop(std::stop_token const& stopToken);
    rs::Result<> fillUntil(std::uint32_t targetBufferedMs, std::uint64_t generation);
    rs::Result<bool> decodeNextBlock(std::uint64_t generation, std::stop_token const* stopToken);
    bool writeBlock(std::span<std::byte const> bytes, std::uint64_t generation, std::stop_token const* stopToken);

    std::unique_ptr<decoder::IAudioDecoderSession> _decoder;
    decoder::DecodedStreamInfo _streamInfo;
    std::function<void(rs::Error const&)> _onError;
    PcmRingBuffer _ringBuffer;
    std::jthread _decodeThread;
    mutable std::mutex _decoderMutex;
    std::atomic<std::uint64_t> _generation = 1;
    std::atomic<bool> _decoderReachedEof = false;
    std::atomic<bool> _failed = false;
    std::uint64_t _bytesPerSecond = 0;
    std::uint32_t _prerollTargetMs = 0;
    std::uint32_t _decodeHighWatermarkMs = 0;
  };

} // namespace app::core::source
