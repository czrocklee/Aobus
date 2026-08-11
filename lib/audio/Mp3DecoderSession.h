// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

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
  class [[nodiscard]] Mp3DecoderSession final : public detail::DecoderSessionBase<Mp3DecoderSession>
  {
  public:
    ~Mp3DecoderSession() override;

    // Not copyable or movable
    Mp3DecoderSession(Mp3DecoderSession const&) = delete;
    Mp3DecoderSession& operator=(Mp3DecoderSession const&) = delete;
    Mp3DecoderSession(Mp3DecoderSession&&) = delete;
    Mp3DecoderSession& operator=(Mp3DecoderSession&&) = delete;

    Result<> seek(std::chrono::milliseconds offset) noexcept override;
    void flush() noexcept override;

    Result<PcmBlock> readNextBlock() noexcept override;
    DecodedStreamInfo streamInfo() const noexcept override;

  private:
    friend class detail::DecoderSessionBase<Mp3DecoderSession>;

    explicit Mp3DecoderSession(std::optional<SampleEncoding> optOutputEncoding);
    Result<> initialize(std::filesystem::path const& filePath) noexcept;

    struct Impl;
    std::unique_ptr<Impl> _implPtr;
  };
} // namespace ao::audio
