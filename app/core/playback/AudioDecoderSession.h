// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include "core/playback/PlaybackTypes.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace app::core::playback
{

  struct PcmBlock final
  {
    std::vector<std::byte> bytes;
    std::uint8_t bitDepth = 16;
    std::uint32_t frames = 0;
    std::uint64_t firstFrameIndex = 0;
    bool endOfStream = false;
  };

  struct DecodedStreamInfo final
  {
    StreamFormat sourceFormat;
    StreamFormat outputFormat;
    std::uint32_t durationMs = 0;
    bool isLossy = false;
  };

  class IAudioDecoderSession
  {
  public:
    virtual ~IAudioDecoderSession() = default;

    virtual bool open(std::filesystem::path const& filePath) = 0;
    virtual void close() = 0;
    virtual bool seek(std::uint32_t positionMs) = 0;
    virtual void flush() = 0;

    virtual std::optional<PcmBlock> readNextBlock() = 0;
    virtual DecodedStreamInfo streamInfo() const = 0;
    virtual std::string_view lastError() const noexcept = 0;
  };

  void initializeAudioDecoders();
  std::unique_ptr<IAudioDecoderSession> createAudioDecoderSession(StreamFormat outputFormat);

} // namespace app::core::playback
