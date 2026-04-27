// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include "core/playback/AudioDecoderSession.h"

namespace app::core::playback
{

  class AlacDecoderSession final : public IAudioDecoderSession
  {
  public:
    explicit AlacDecoderSession(StreamFormat outputFormat);
    ~AlacDecoderSession() override;

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

} // namespace app::core::playback
