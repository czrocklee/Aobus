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
    // Raw extensible mask DWORD; ignored for legacy formats with no mask slot.
    std::uint32_t channelMask = 0;
    std::vector<std::uint8_t> audioData = {0, 0};
    std::vector<InfoField> infoFields = {};
    std::vector<Chunk> extraChunks = {};
  };

  std::vector<std::uint8_t> makeId3WithPicture(std::span<std::uint8_t const> imageData);

  // Builds a v2.3 ID3 payload with the tag-level unsynchronisation flag set,
  // carrying a TIT2 frame and a front-cover APIC frame. Both the title and the
  // image data are stored bytes: FF 00 escape pairs stay in, and frame header
  // sizes declare the deunsynchronised body size.
  std::vector<std::uint8_t> makeUnsyncId3WithPictureAndTitle(std::span<std::uint8_t const> imageData,
                                                             std::string_view storedTitle);

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

  // Throws std::overflow_error if the computed byte rate cannot fit its DWORD.
  // Deliberately incoherent rates must be authored in a raw fmt payload passed
  // to the makeWav overload. This helper does not depend on a test framework.
  std::vector<std::uint8_t> makeFmtChunk(Spec const& spec);

  std::vector<std::uint8_t> makeInfoList(std::span<InfoField const> fields);

  // Builds a complete RIFF WAVE file with the same framing as the
  // single-argument makeWav, but the fmt chunk carries the supplied payload
  // bytes verbatim; the Spec supplies the audio data and any optional INFO
  // and extra chunks.
  std::vector<std::uint8_t> makeWav(Spec const& spec, std::span<std::uint8_t const> fmtPayload);

  std::vector<std::uint8_t> makeWav(Spec const& spec);
} // namespace ao::test::wav
