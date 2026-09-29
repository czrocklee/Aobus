// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/media/wav/TestWav.h"

#include "lib/media/file/mpeg/id3v2/Layout.h"
#include "test/unit/media/file/id3v2/TestId3v2.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

namespace ao::test::wav
{
  namespace
  {
    namespace id3v2 = ao::media::file::mpeg::id3v2;

    using ao::test::id3v2::deunsynchronisedSize;

    void appendV23Frame(std::vector<std::uint8_t>& data,
                        std::string_view const id,
                        std::span<std::uint8_t const> const body,
                        std::size_t const declaredSize)
    {
      auto frame = id3v2::V23CommonFrameLayout{};
      std::memcpy(frame.id.data(), id.data(), frame.id.size());
      frame.size = static_cast<std::uint32_t>(declaredSize);
      auto const* const frameBytes = reinterpret_cast<std::uint8_t const*>(&frame);
      data.insert(data.end(), frameBytes, frameBytes + sizeof(frame));
      data.insert(data.end(), body.begin(), body.end());
    }

    void appendId3Header(std::vector<std::uint8_t>& data, std::uint8_t const flags, std::size_t const bodySize)
    {
      auto header = id3v2::HeaderLayout{};
      std::memcpy(header.id.data(), "ID3", header.id.size());
      header.majorVersion = 3;
      header.flags = flags;

      auto const size = static_cast<std::uint32_t>(bodySize);
      header.size.data[0] = static_cast<std::uint8_t>((size >> 21U) & 0x7FU);
      header.size.data[1] = static_cast<std::uint8_t>((size >> 14U) & 0x7FU);
      header.size.data[2] = static_cast<std::uint8_t>((size >> 7U) & 0x7FU);
      header.size.data[3] = static_cast<std::uint8_t>(size & 0x7FU);

      auto const* const headerBytes = reinterpret_cast<std::uint8_t const*>(&header);
      data.insert(data.end(), headerBytes, headerBytes + sizeof(header));
    }

    std::vector<std::uint8_t> pictureFrameBody(std::span<std::uint8_t const> const imageData)
    {
      auto body = std::vector<std::uint8_t>{0}; // Latin1
      body.insert(body.end(), {'i', 'm', 'a', 'g', 'e', '/', 'p', 'n', 'g', 0});
      body.insert(body.end(), {3, 0}); // Front cover and empty description
      body.insert(body.end(), imageData.begin(), imageData.end());
      return body;
    }

    void addPictureFrame(std::vector<std::uint8_t>& data, std::span<std::uint8_t const> const imageData)
    {
      auto const body = pictureFrameBody(imageData);
      appendV23Frame(data, "APIC", body, body.size());
    }
  } // namespace

  std::vector<std::uint8_t> makeId3WithPicture(std::span<std::uint8_t const> const imageData)
  {
    auto body = std::vector<std::uint8_t>{};
    addPictureFrame(body, imageData);

    auto data = std::vector<std::uint8_t>{};
    appendId3Header(data, 0, body.size());
    data.insert(data.end(), body.begin(), body.end());
    return data;
  }

  std::vector<std::uint8_t> makeUnsyncId3WithPictureAndTitle(std::span<std::uint8_t const> const imageData,
                                                             std::string_view const storedTitle)
  {
    auto titleBody = std::vector<std::uint8_t>{0}; // Latin1

    for (char const character : storedTitle)
    {
      titleBody.push_back(static_cast<std::uint8_t>(character));
    }

    auto const pictureBody = pictureFrameBody(imageData);
    auto body = std::vector<std::uint8_t>{};
    appendV23Frame(body, "TIT2", titleBody, deunsynchronisedSize(titleBody));
    appendV23Frame(body, "APIC", pictureBody, deunsynchronisedSize(pictureBody));

    auto data = std::vector<std::uint8_t>{};
    appendId3Header(data, 0x80, body.size()); // tag-level unsynchronisation
    data.insert(data.end(), body.begin(), body.end());
    return data;
  }

  void appendId(std::vector<std::uint8_t>& output, std::array<char, 4> const& id)
  {
    output.insert(output.end(), id.begin(), id.end());
  }

  void appendId(std::vector<std::uint8_t>& output, std::string_view const id)
  {
    for (char const ch : id)
    {
      output.push_back(static_cast<std::uint8_t>(ch));
    }
  }

  void appendLe16(std::vector<std::uint8_t>& output, std::uint16_t const value)
  {
    output.push_back(static_cast<std::uint8_t>(value & 0xFFU));
    output.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
  }

  void appendLe32(std::vector<std::uint8_t>& output, std::uint32_t const value)
  {
    output.push_back(static_cast<std::uint8_t>(value & 0xFFU));
    output.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
    output.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xFFU));
    output.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xFFU));
  }

  void appendChunk(std::vector<std::uint8_t>& output,
                   std::array<char, 4> const& id,
                   std::span<std::uint8_t const> const payload)
  {
    appendId(output, id);
    appendLe32(output, static_cast<std::uint32_t>(payload.size()));
    output.insert(output.end(), payload.begin(), payload.end());

    if ((payload.size() % 2U) != 0U)
    {
      output.push_back(0);
    }
  }

  void appendChunk(std::vector<std::uint8_t>& output,
                   std::string_view const id,
                   std::span<std::uint8_t const> const payload)
  {
    appendChunk(output, {id[0], id[1], id[2], id[3]}, payload);
  }

  void appendTruncatedChunk(std::vector<std::uint8_t>& riff,
                            std::string_view const id,
                            std::uint32_t const declaredSize)
  {
    appendId(riff, id);
    appendLe32(riff, declaredSize);
    auto const riffSize = static_cast<std::uint32_t>(riff.size() - 8U);
    riff[4] = static_cast<std::uint8_t>(riffSize & 0xFFU);
    riff[5] = static_cast<std::uint8_t>((riffSize >> 8U) & 0xFFU);
    riff[6] = static_cast<std::uint8_t>((riffSize >> 16U) & 0xFFU);
    riff[7] = static_cast<std::uint8_t>((riffSize >> 24U) & 0xFFU);
  }

  void appendGuid(std::vector<std::uint8_t>& output, SampleFormat const sampleFormat)
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

  std::vector<std::uint8_t> makeFmtChunk(Spec const& spec)
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

  std::vector<std::uint8_t> makeInfoList(std::span<InfoField const> const fields)
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

  std::vector<std::uint8_t> makeWav(Spec const& spec)
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
