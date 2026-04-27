// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include "core/decoder/IAudioDecoderSession.h"

namespace app::core::decoder
{

  class FlacDecoderSession final : public IAudioDecoderSession
  {
  public:
    explicit FlacDecoderSession(playback::StreamFormat outputFormat);
    ~FlacDecoderSession() override;

    bool open(std::filesystem::path const& filePath) override;
    void close() override;
    bool seek(std::uint32_t positionMs) override;
    void flush() override;

    std::optional<PcmBlock> readNextBlock() override;
    DecodedStreamInfo streamInfo() const override;
    std::string_view lastError() const noexcept override;

  private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
  };

} // namespace app::core::decoder