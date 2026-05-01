// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <rs/audio/IDecoderSession.h>

namespace rs::audio
{
  class AlacDecoderSession final : public IDecoderSession
  {
  public:
    explicit AlacDecoderSession(Format outputFormat);
    ~AlacDecoderSession() override;

    rs::Result<> open(std::filesystem::path const& filePath) override;
    void close() override;
    rs::Result<> seek(std::uint32_t positionMs) override;
    void flush() override;

    rs::Result<PcmBlock> readNextBlock() override;
    DecodedStreamInfo streamInfo() const override;

  private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
  };
} // namespace rs::audio