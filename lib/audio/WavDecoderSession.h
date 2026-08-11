// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "detail/DecoderSessionBase.h"
#include <ao/Error.h>
#include <ao/audio/DecodedStreamInfo.h>
#include <ao/audio/PcmBlock.h>
#include <ao/audio/SampleEncoding.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>

namespace ao::audio
{
  class [[nodiscard]] WavDecoderSession final : public detail::DecoderSessionBase<WavDecoderSession>
  {
  public:
    ~WavDecoderSession() override;

    WavDecoderSession(WavDecoderSession const&) = delete;
    WavDecoderSession& operator=(WavDecoderSession const&) = delete;
    WavDecoderSession(WavDecoderSession&&) = delete;
    WavDecoderSession& operator=(WavDecoderSession&&) = delete;

    Result<> seek(std::chrono::milliseconds offset) noexcept override;
    void flush() noexcept override;

    Result<PcmBlock> readNextBlock() noexcept override;
    DecodedStreamInfo streamInfo() const noexcept override;

  private:
    friend class detail::DecoderSessionBase<WavDecoderSession>;

    explicit WavDecoderSession(std::optional<SampleEncoding> optOutputEncoding);
    Result<> initialize(std::filesystem::path const& filePath) noexcept;

    struct Impl;
    std::unique_ptr<Impl> _implPtr;
  };
} // namespace ao::audio
