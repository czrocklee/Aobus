// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/audio/DecoderTypes.h>
#include <ao/audio/Format.h>
#include <ao/audio/detail/DecoderSessionBase.h>

#include <chrono>
#include <filesystem>
#include <memory>

namespace ao::audio
{
  class [[nodiscard]] FlacDecoderSession final : public detail::DecoderSessionBase<FlacDecoderSession>
  {
  public:
    explicit FlacDecoderSession(Format outputFormat);
    ~FlacDecoderSession() override;

    // Not copyable or movable
    FlacDecoderSession(FlacDecoderSession const&) = delete;
    FlacDecoderSession& operator=(FlacDecoderSession const&) = delete;
    FlacDecoderSession(FlacDecoderSession&&) = delete;
    FlacDecoderSession& operator=(FlacDecoderSession&&) = delete;

    Result<> openCodec(std::filesystem::path const& filePath);
    void close() override;
    Result<> seek(std::chrono::milliseconds offset) override;
    void flush() override;

    Result<PcmBlock> readNextBlock() override;
    DecodedStreamInfo streamInfo() const override;

  private:
    struct Impl;
    std::unique_ptr<Impl> _implPtr;
  };
} // namespace ao::audio