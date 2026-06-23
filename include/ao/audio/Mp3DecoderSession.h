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
  class [[nodiscard]] Mp3DecoderSession final : public detail::DecoderSessionBase<Mp3DecoderSession>
  {
  public:
    explicit Mp3DecoderSession(Format outputFormat);
    ~Mp3DecoderSession() override;

    // Not copyable or movable
    Mp3DecoderSession(Mp3DecoderSession const&) = delete;
    Mp3DecoderSession& operator=(Mp3DecoderSession const&) = delete;
    Mp3DecoderSession(Mp3DecoderSession&&) = delete;
    Mp3DecoderSession& operator=(Mp3DecoderSession&&) = delete;

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
