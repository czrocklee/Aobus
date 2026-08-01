// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/audio/DecodedStreamInfo.h>
#include <ao/audio/PcmBlock.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/detail/DecoderSessionBase.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>

namespace ao::audio
{
  class [[nodiscard]] WavDecoderSession final : public detail::DecoderSessionBase<WavDecoderSession>
  {
  public:
    explicit WavDecoderSession(std::optional<SampleEncoding> optOutputEncoding);
    ~WavDecoderSession() override;

    WavDecoderSession(WavDecoderSession const&) = delete;
    WavDecoderSession& operator=(WavDecoderSession const&) = delete;
    WavDecoderSession(WavDecoderSession&&) = delete;
    WavDecoderSession& operator=(WavDecoderSession&&) = delete;

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
