// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/audio/DecodedStreamInfo.h>
#include <ao/audio/Format.h>
#include <ao/audio/PcmBlock.h>
#include <ao/audio/detail/DecoderSessionBase.h>

#include <chrono>
#include <filesystem>
#include <memory>

namespace ao::audio
{
  class [[nodiscard]] AacDecoderSession final : public detail::DecoderSessionBase<AacDecoderSession>
  {
  public:
    explicit AacDecoderSession(Format outputFormat);
    ~AacDecoderSession() override;

    AacDecoderSession(AacDecoderSession const&) = delete;
    AacDecoderSession& operator=(AacDecoderSession const&) = delete;
    AacDecoderSession(AacDecoderSession&&) = delete;
    AacDecoderSession& operator=(AacDecoderSession&&) = delete;

    Result<> openCodec(std::filesystem::path const& filePath);
    void close() noexcept override;
    Result<> seek(std::chrono::milliseconds offset) noexcept override;
    void flush() noexcept override;

    Result<PcmBlock> readNextBlock() noexcept override;
    DecodedStreamInfo streamInfo() const noexcept override;

  private:
    struct Impl;
    std::unique_ptr<Impl> _implPtr;
  };
} // namespace ao::audio
