// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/audio/Format.h>
#include <ao/audio/IDecoderSession.h>

#include <filesystem>
#include <memory>

namespace ao::audio
{
  class FlacDecoderSession final : public IDecoderSession
  {
  public:
    explicit FlacDecoderSession(Format outputFormat);
    ~FlacDecoderSession() override;

    // Not copyable or movable
    FlacDecoderSession(FlacDecoderSession const&) = delete;
    FlacDecoderSession& operator=(FlacDecoderSession const&) = delete;
    FlacDecoderSession(FlacDecoderSession&&) = delete;
    FlacDecoderSession& operator=(FlacDecoderSession&&) = delete;

    Result<> open(std::filesystem::path const& filePath) override;
    void close() override;
    Result<> seek(std::uint32_t positionMs) override;
    void flush() override;

    Result<PcmBlock> readNextBlock() override;
    DecodedStreamInfo streamInfo() const override;

  private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
  };
} // namespace ao::audio