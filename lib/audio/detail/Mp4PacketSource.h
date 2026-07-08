// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/media/mp4/Demuxer.h>
#include <ao/utility/MappedFile.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string_view>

namespace ao::audio::detail
{
  class Mp4PacketSource final
  {
  public:
    Result<> open(std::filesystem::path const& filePath, std::string_view sampleEntry);
    void close() noexcept;

    Result<> seek(std::chrono::milliseconds offset, std::uint32_t fallbackTimescale = 0);

    bool isOpen() const noexcept;
    bool isAtEnd() const noexcept;
    std::span<std::byte const> packet() const;
    std::span<std::byte const> magicCookie() const;
    media::mp4::Demuxer::SampleEntry sampleInfo() const;
    std::uint32_t sampleIndex() const noexcept;
    std::uint32_t timescale(std::uint32_t fallback = 0) const noexcept;
    std::chrono::milliseconds duration(std::uint32_t fallbackTimescale = 0) const noexcept;
    std::uint64_t firstFrameIndex(std::uint32_t sampleRate, std::uint32_t fallbackFramesPerPacket) const noexcept;

    void advance() noexcept;

  private:
    utility::MappedFile _mappedFile;
    std::unique_ptr<media::mp4::Demuxer> _demuxerPtr;
    std::uint32_t _sampleIndex = 0;
  };
} // namespace ao::audio::detail
