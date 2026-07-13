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

  inline void appendId(std::vector<std::uint8_t>& output, std::array<char, 4> const& id)
  {
    output.insert(output.end(), id.begin(), id.end());
  }

  inline void appendId(std::vector<std::uint8_t>& output, std::string_view id)
  {
    for (char const ch : id)
    {
      output.push_back(static_cast<std::uint8_t>(ch));
    }
  }

  inline void appendLe16(std::vector<std::uint8_t>& output, std::uint16_t value)
  {
    output.push_back(static_cast<std::uint8_t>(value & 0xFFU));
    output.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
  }

  inline void appendLe32(std::vector<std::uint8_t>& output, std::uint32_t value)
  {
    output.push_back(static_cast<std::uint8_t>(value & 0xFFU));
    output.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
    output.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xFFU));
    output.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xFFU));
  }

  inline void appendChunk(std::vector<std::uint8_t>& output,
                          std::array<char, 4> const& id,
                          std::span<std::uint8_t const> payload)
  {
    appendId(output, id);
    appendLe32(output, static_cast<std::uint32_t>(payload.size()));
    output.insert(output.end(), payload.begin(), payload.end());

    if ((payload.size() % 2U) != 0U)
    {
      output.push_back(0);
    }
  }

  inline void appendChunk(std::vector<std::uint8_t>& output, std::string_view id, std::span<std::uint8_t const> payload)
  {
    appendChunk(output, {id[0], id[1], id[2], id[3]}, payload);
  }

  inline void appendTruncatedChunk(std::vector<std::uint8_t>& riff, std::string_view id, std::uint32_t declaredSize)
  {
    appendId(riff, id);
    appendLe32(riff, declaredSize);
    auto const riffSize = static_cast<std::uint32_t>(riff.size() - 8U);
    riff[4] = static_cast<std::uint8_t>(riffSize & 0xFFU);
    riff[5] = static_cast<std::uint8_t>((riffSize >> 8U) & 0xFFU);
    riff[6] = static_cast<std::uint8_t>((riffSize >> 16U) & 0xFFU);
    riff[7] = static_cast<std::uint8_t>((riffSize >> 24U) & 0xFFU);
  }

  inline void appendGuid(std::vector<std::uint8_t>& output, SampleFormat sampleFormat)
  {
    std::uint8_t first = 0x01;

    if (sampleFormat == SampleFormat::ExtensibleFloat)
    {
      first = 0x03;
    }
    else if (sampleFormat == SampleFormat::UnsupportedExtensible)
    {
      first = 0xFF;
    }

    output.insert(
      output.end(), {first, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71});
  }

  inline std::vector<std::uint8_t> makeFmtChunk(Spec const& spec)
  {
    auto payload = std::vector<std::uint8_t>{};
    auto const formatTag = [&]
    {
      switch (spec.sampleFormat)
      {
        case SampleFormat::Pcm: return std::uint16_t{0x0001};
        case SampleFormat::IeeeFloat: return std::uint16_t{0x0003};
        case SampleFormat::ExtensiblePcm:
        case SampleFormat::ExtensibleFloat:
        case SampleFormat::UnsupportedExtensible: return std::uint16_t{0xFFFE};
      }

      return std::uint16_t{0x0001};
    }();
    auto const bytesPerSample = static_cast<std::uint16_t>((spec.bitsPerSample + 7U) / 8U);
    auto const blockAlign = static_cast<std::uint16_t>(spec.channels * bytesPerSample);

    appendLe16(payload, formatTag);
    appendLe16(payload, spec.channels);
    appendLe32(payload, spec.sampleRate);
    appendLe32(payload, spec.sampleRate * blockAlign);
    appendLe16(payload, blockAlign);
    appendLe16(payload, spec.bitsPerSample);

    if (formatTag == 0xFFFE)
    {
      appendLe16(payload, 22);
      appendLe16(payload, spec.validBitsPerSample == 0 ? spec.bitsPerSample : spec.validBitsPerSample);
      appendLe32(payload, 0);
      appendGuid(payload, spec.sampleFormat);
    }

    return payload;
  }

  inline std::vector<std::uint8_t> makeInfoList(std::span<InfoField const> fields)
  {
    auto payload = std::vector<std::uint8_t>{'I', 'N', 'F', 'O'};

    for (auto const& field : fields)
    {
      auto value = std::vector<std::uint8_t>{field.value.begin(), field.value.end()};
      value.push_back(0);
      appendChunk(payload, field.id, value);
    }

    return payload;
  }

  inline std::vector<std::uint8_t> makeWav(Spec const& spec)
  {
    auto body = std::vector<std::uint8_t>{'W', 'A', 'V', 'E'};
    auto fmt = makeFmtChunk(spec);
    appendChunk(body, "fmt ", fmt);

    if (!spec.infoFields.empty())
    {
      auto info = makeInfoList(spec.infoFields);
      appendChunk(body, "LIST", info);
    }

    for (auto const& chunk : spec.extraChunks)
    {
      appendChunk(body, chunk.id, chunk.payload);
    }

    appendChunk(body, "data", spec.audioData);

    auto riff = std::vector<std::uint8_t>{};
    appendId(riff, "RIFF");
    appendLe32(riff, static_cast<std::uint32_t>(body.size()));
    riff.insert(riff.end(), body.begin(), body.end());
    return riff;
  }
} // namespace ao::test::wav
