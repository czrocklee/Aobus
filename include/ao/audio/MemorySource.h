// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <ao/audio/IDecoderSession.h>
#include <ao/audio/ISource.h>

#include <atomic>
#include <cstddef>
#include <string>
#include <vector>

namespace ao::audio
{
  class MemorySource final : public ISource
  {
  public:
    MemorySource(std::unique_ptr<IDecoderSession> decoder, DecodedStreamInfo streamInfo);

    MemorySource(MemorySource const&) = delete;
    MemorySource& operator=(MemorySource const&) = delete;

    ao::Result<> initialize();
    ao::Result<> seek(std::uint32_t positionMs) override;

    std::size_t read(std::span<std::byte> output) noexcept override;
    bool isDrained() const noexcept override;
    std::uint32_t bufferedMs() const noexcept override;

  private:
    std::size_t positionToByteOffset(std::uint32_t positionMs) const noexcept;

    std::unique_ptr<IDecoderSession> _decoder;
    DecodedStreamInfo _streamInfo;
    std::vector<std::byte> _pcmBytes;
    std::atomic<std::size_t> _readOffset{0};
  };
} // namespace ao::audio
