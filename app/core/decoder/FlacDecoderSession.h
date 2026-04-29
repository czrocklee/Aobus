// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include "core/decoder/IAudioDecoderSession.h"

namespace app::core::decoder
{

  class FlacDecoderSession final : public IAudioDecoderSession
  {
  public:
    explicit FlacDecoderSession(AudioFormat outputFormat);
    ~FlacDecoderSession() override;

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

} // namespace app::core::decoder