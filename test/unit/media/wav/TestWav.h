// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ao::test::wav
{
  enum class SampleFormat : std::uint8_t
  {
    Pcm,
    IeeeFloat,
    ExtensiblePcm,
    ExtensibleFloat,
    UnsupportedExtensible,
  };

  struct InfoField final
  {
    std::array<char, 4> id{};
    std::string value{};
  };

  struct Chunk final
  {
    std::array<char, 4> id{};
    std::vector<std::uint8_t> payload{};
  };

  struct Spec final
  {
    SampleFormat sampleFormat = SampleFormat::Pcm;
    std::uint16_t channels = 1;
    std::uint32_t sampleRate = 1000;
    std::uint16_t bitsPerSample = 16;
    std::uint16_t validBitsPerSample = 0;
    std::vector<std::uint8_t> audioData = {0, 0};
    std::vector<InfoField> infoFields = {};
    std::vector<Chunk> extraChunks = {};
  };

  void appendId(std::vector<std::uint8_t>& output, std::array<char, 4> const& id);

  void appendId(std::vector<std::uint8_t>& output, std::string_view id);

  void appendLe16(std::vector<std::uint8_t>& output, std::uint16_t value);

  void appendLe32(std::vector<std::uint8_t>& output, std::uint32_t value);

  void appendChunk(std::vector<std::uint8_t>& output,
                   std::array<char, 4> const& id,
                   std::span<std::uint8_t const> payload);

  void appendChunk(std::vector<std::uint8_t>& output, std::string_view id, std::span<std::uint8_t const> payload);

  void appendTruncatedChunk(std::vector<std::uint8_t>& riff, std::string_view id, std::uint32_t declaredSize);

  void appendGuid(std::vector<std::uint8_t>& output, SampleFormat sampleFormat);

  std::vector<std::uint8_t> makeFmtChunk(Spec const& spec);

  std::vector<std::uint8_t> makeInfoList(std::span<InfoField const> fields);

  std::vector<std::uint8_t> makeWav(Spec const& spec);
} // namespace ao::test::wav
