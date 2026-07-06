// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace ao::media::wav
{
  inline constexpr std::uint16_t kFormatPcm = 0x0001;
  inline constexpr std::uint16_t kFormatIeeeFloat = 0x0003;
  inline constexpr std::uint16_t kFormatExtensible = 0xFFFE;

  struct FormatChunk final
  {
    std::uint16_t formatTag = 0;
    std::uint16_t channels = 0;
    std::uint32_t sampleRate = 0;
    std::uint32_t byteRate = 0;
    std::uint16_t blockAlign = 0;
    std::uint16_t bitsPerSample = 0;
    std::uint16_t validBitsPerSample = 0;
    bool isFloat = false;
  };

  struct ChunkView final
  {
    std::array<char, 4> id{};
    std::size_t offset = 0;
    std::span<std::byte const> bytes{};
  };

  struct ParsedWave final
  {
    FormatChunk format{};
    std::size_t dataOffset = 0;
    std::span<std::byte const> data{};
    std::vector<ChunkView> chunks{};
  };

  Result<ParsedWave> parseWave(std::span<std::byte const> bytes);

  bool chunkIdEquals(ChunkView const& chunk, std::string_view id) noexcept;
} // namespace ao::media::wav
