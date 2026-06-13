// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/audio/DecoderTypes.h>
#include <ao/audio/Format.h>
#include <ao/audio/IDecoderSession.h>

#include <chrono>
#include <filesystem>
#include <memory>

namespace ao::audio
{
  class [[nodiscard]] AacDecoderSession final : public IDecoderSession
  {
  public:
    explicit AacDecoderSession(Format outputFormat);
    ~AacDecoderSession() override;

    AacDecoderSession(AacDecoderSession const&) = delete;
    AacDecoderSession& operator=(AacDecoderSession const&) = delete;
    AacDecoderSession(AacDecoderSession&&) = delete;
    AacDecoderSession& operator=(AacDecoderSession&&) = delete;

    Result<> open(std::filesystem::path const& filePath) override;
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
