// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include "core/decoder/IAudioDecoderSession.h"
#include "core/source/IPcmSource.h"

#include <cstddef>
#include <mutex>
#include <string>
#include <vector>

namespace app::core::source
{

  class MemoryPcmSource final : public IPcmSource
  {
  public:
    MemoryPcmSource(std::unique_ptr<decoder::IAudioDecoderSession> decoder, decoder::DecodedStreamInfo streamInfo);

    rs::Result<> initialize();
    rs::Result<> seek(std::uint32_t positionMs) override;

    std::size_t read(std::span<std::byte> output) noexcept override;
    bool isDrained() const noexcept override;
    std::uint32_t bufferedMs() const noexcept override;

  private:
    std::size_t positionToByteOffset(std::uint32_t positionMs) const noexcept;

    std::unique_ptr<decoder::IAudioDecoderSession> _decoder;
    decoder::DecodedStreamInfo _streamInfo;
    std::vector<std::byte> _pcmBytes;
    mutable std::mutex _mutex;
    std::size_t _readOffset = 0;
  };

} // namespace app::core::playback
