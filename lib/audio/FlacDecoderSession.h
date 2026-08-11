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
  class [[nodiscard]] FlacDecoderSession final : public detail::DecoderSessionBase<FlacDecoderSession>
  {
  public:
    ~FlacDecoderSession() override;

    // Not copyable or movable
    FlacDecoderSession(FlacDecoderSession const&) = delete;
    FlacDecoderSession& operator=(FlacDecoderSession const&) = delete;
    FlacDecoderSession(FlacDecoderSession&&) = delete;
    FlacDecoderSession& operator=(FlacDecoderSession&&) = delete;

    Result<> seek(std::chrono::milliseconds offset) noexcept override;
    void flush() noexcept override;

    Result<PcmBlock> readNextBlock() noexcept override;
    DecodedStreamInfo streamInfo() const noexcept override;

  private:
    friend class detail::DecoderSessionBase<FlacDecoderSession>;

    explicit FlacDecoderSession(std::optional<SampleEncoding> optOutputEncoding);
    Result<> initialize(std::filesystem::path const& filePath) noexcept;

    struct Impl;
    std::unique_ptr<Impl> _implPtr;
  };
} // namespace ao::audio
