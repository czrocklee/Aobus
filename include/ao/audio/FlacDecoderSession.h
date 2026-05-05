// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/audio/IDecoderSession.h>

namespace ao::audio
{
  class FlacDecoderSession final : public IDecoderSession
  {
  public:
    explicit FlacDecoderSession(Format outputFormat);
    ~FlacDecoderSession() override;

    ao::Result<> open(std::filesystem::path const& filePath) override;
    void close() override;
    ao::Result<> seek(std::uint32_t positionMs) override;
    void flush() override;

    ao::Result<PcmBlock> readNextBlock() override;
    DecodedStreamInfo streamInfo() const override;

  private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
  };
} // namespace ao::audio