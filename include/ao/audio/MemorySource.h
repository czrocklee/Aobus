// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/audio/DecoderTypes.h>
#include <ao/audio/IDecoderSession.h>
#include <ao/audio/ISource.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace ao::audio
{
  class MemorySource final : public ISource
  {
  public:
    MemorySource(std::unique_ptr<IDecoderSession> decoderPtr, DecodedStreamInfo streamInfo);

    MemorySource(MemorySource const&) = delete;
    MemorySource& operator=(MemorySource const&) = delete;
    MemorySource(MemorySource&&) = delete;
    MemorySource& operator=(MemorySource&&) = delete;

    ~MemorySource() override = default;

    Result<> initialize();
    Result<> seek(std::chrono::milliseconds offset) override;

    std::size_t read(std::span<std::byte> output) noexcept override;
    bool isDrained() const noexcept override;
    std::chrono::milliseconds bufferedDuration() const noexcept override;

  private:
    std::size_t timeToByteOffset(std::chrono::milliseconds offset) const noexcept;

    std::unique_ptr<IDecoderSession> _decoderPtr;
    DecodedStreamInfo _streamInfo;
    std::vector<std::byte> _pcmBytes;
    std::atomic<std::size_t> _readOffset{0};
    std::uint64_t _bytesPerSecond{0};
  };
} // namespace ao::audio
