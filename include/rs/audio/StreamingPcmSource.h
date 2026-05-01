// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <rs/audio/IAudioDecoderSession.h>
#include <rs/audio/IPcmSource.h>
#include <rs/audio/PcmRingBuffer.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>

namespace rs::audio
{
  class StreamingPcmSource final : public IPcmSource
  {
  public:
    StreamingPcmSource(std::unique_ptr<rs::audio::IAudioDecoderSession> decoder,
                       rs::audio::DecodedStreamInfo streamInfo,
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

    std::unique_ptr<rs::audio::IAudioDecoderSession> _decoder;
    rs::audio::DecodedStreamInfo _streamInfo;
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
} // namespace rs::audio
