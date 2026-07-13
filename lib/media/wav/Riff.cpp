// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/Error.h>
#include <ao/media/wav/Riff.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <span>
#include <string_view>

namespace ao::media::wav
{
  namespace
  {
    constexpr std::size_t kRiffHeaderSize = 12;
    constexpr std::size_t kChunkHeaderSize = 8;
    constexpr std::size_t kBaseFormatSize = 16;
    constexpr std::size_t kFormatTagOffset = 0;
    constexpr std::size_t kFormatChannelsOffset = 2;
    constexpr std::size_t kFormatSampleRateOffset = 4;
    constexpr std::size_t kFormatByteRateOffset = 8;
    constexpr std::size_t kFormatBlockAlignOffset = 12;
    constexpr std::size_t kFormatBitsPerSampleOffset = 14;
    constexpr std::size_t kFormatExtensionSizeOffset = 16;
    constexpr std::size_t kExtensibleValidBitsOffset = 18;
    constexpr std::size_t kExtensibleSubFormatOffset = 24;
    constexpr std::uint16_t kExtensibleMinimumExtensionSize = 22;

    constexpr auto kPcmGuid = std::to_array<std::byte>({
      std::byte{0x01},
      std::byte{0x00},
      std::byte{0x00},
      std::byte{0x00},
      std::byte{0x00},
      std::byte{0x00},
      std::byte{0x10},
      std::byte{0x00},
      std::byte{0x80},
      std::byte{0x00},
      std::byte{0x00},
      std::byte{0xAA},
      std::byte{0x00},
      std::byte{0x38},
      std::byte{0x9B},
      std::byte{0x71},
    });

    constexpr auto kIeeeFloatGuid = std::to_array<std::byte>({
      std::byte{0x03},
      std::byte{0x00},
      std::byte{0x00},
      std::byte{0x00},
      std::byte{0x00},
      std::byte{0x00},
      std::byte{0x10},
      std::byte{0x00},
      std::byte{0x80},
      std::byte{0x00},
      std::byte{0x00},
      std::byte{0xAA},
      std::byte{0x00},
      std::byte{0x38},
      std::byte{0x9B},
      std::byte{0x71},
    });

    bool matchesByteText(std::span<std::byte const> bytes, std::string_view text) noexcept
    {
      if (bytes.size() != text.size())
      {
        return false;
      }

      for (std::size_t index = 0; index < bytes.size(); ++index)
      {
        if (std::to_integer<unsigned char>(bytes[index]) != static_cast<unsigned char>(text[index]))
        {
          return false;
        }
      }

      return true;
    }

    std::uint16_t readLe16(std::span<std::byte const> bytes, std::size_t offset) noexcept
    {
      return static_cast<std::uint16_t>(std::to_integer<std::uint16_t>(bytes[offset])) |
             static_cast<std::uint16_t>(std::to_integer<std::uint16_t>(bytes[offset + 1]) << 8U);
    }

    std::uint32_t readLe32(std::span<std::byte const> bytes, std::size_t offset) noexcept
    {
      return static_cast<std::uint32_t>(std::to_integer<std::uint32_t>(bytes[offset])) |
             (static_cast<std::uint32_t>(std::to_integer<std::uint32_t>(bytes[offset + 1])) << 8U) |
             (static_cast<std::uint32_t>(std::to_integer<std::uint32_t>(bytes[offset + 2])) << 16U) |
             (static_cast<std::uint32_t>(std::to_integer<std::uint32_t>(bytes[offset + 3])) << 24U);
    }

    bool isSupportedIntegerDepth(std::uint16_t bits) noexcept
    {
      return bits == 8 || bits == 16 || bits == 24 || bits == 32;
    }

    Result<FormatChunk> parseFormatChunk(std::span<std::byte const> bytes)
    {
      if (bytes.size() < kBaseFormatSize)
      {
        return makeError(Error::Code::CorruptData, "WAV fmt chunk is truncated");
      }

      auto format = FormatChunk{
        .formatTag = readLe16(bytes, kFormatTagOffset),
        .channels = readLe16(bytes, kFormatChannelsOffset),
        .sampleRate = readLe32(bytes, kFormatSampleRateOffset),
        .byteRate = readLe32(bytes, kFormatByteRateOffset),
        .blockAlign = readLe16(bytes, kFormatBlockAlignOffset),
        .bitsPerSample = readLe16(bytes, kFormatBitsPerSampleOffset),
        .validBitsPerSample = readLe16(bytes, kFormatBitsPerSampleOffset),
        .isFloat = false,
      };

      if (format.formatTag == kFormatExtensible)
      {
        if (bytes.size() < kExtensibleSubFormatOffset + kPcmGuid.size() ||
            readLe16(bytes, kFormatExtensionSizeOffset) < kExtensibleMinimumExtensionSize)
        {
          return makeError(Error::Code::CorruptData, "WAV extensible fmt chunk is truncated");
        }

        format.validBitsPerSample = readLe16(bytes, kExtensibleValidBitsOffset);
        auto const subFormat = bytes.subspan(kExtensibleSubFormatOffset, kPcmGuid.size());

        if (std::ranges::equal(subFormat, kPcmGuid))
        {
          format.formatTag = kFormatPcm;
        }
        else if (std::ranges::equal(subFormat, kIeeeFloatGuid))
        {
          format.formatTag = kFormatIeeeFloat;
        }
        else
        {
          return makeError(Error::Code::NotSupported, "Unsupported WAV extensible subformat");
        }

        if (format.validBitsPerSample == 0)
        {
          format.validBitsPerSample = format.bitsPerSample;
        }
      }

      if (format.channels == 0 || format.sampleRate == 0 || format.bitsPerSample == 0 || format.blockAlign == 0 ||
          format.byteRate == 0)
      {
        return makeError(Error::Code::CorruptData, "WAV fmt chunk contains zero audio properties");
      }

      if (format.channels > std::numeric_limits<std::uint8_t>::max())
      {
        return makeError(Error::Code::NotSupported, "Unsupported WAV channel count");
      }

      if (format.validBitsPerSample == 0 || format.validBitsPerSample > format.bitsPerSample)
      {
        return makeError(Error::Code::CorruptData, "WAV fmt chunk has invalid valid-bit depth");
      }

      if (format.validBitsPerSample != format.bitsPerSample)
      {
        return makeError(Error::Code::NotSupported, "Unsupported WAV valid-bit/container mismatch");
      }

      if (format.formatTag == kFormatPcm)
      {
        if (!isSupportedIntegerDepth(format.bitsPerSample))
        {
          return makeError(Error::Code::NotSupported, "Unsupported WAV PCM bit depth");
        }
      }
      else if (format.formatTag == kFormatIeeeFloat)
      {
        if (format.bitsPerSample != 32 || format.validBitsPerSample != 32)
        {
          return makeError(Error::Code::NotSupported, "Unsupported WAV float sample format");
        }

        format.isFloat = true;
      }
      else
      {
        return makeError(Error::Code::NotSupported, "Unsupported WAV format code");
      }

      auto const bytesPerSample = static_cast<std::uint16_t>((format.bitsPerSample + 7U) / 8U);

      if (format.blockAlign != format.channels * bytesPerSample ||
          format.byteRate != format.sampleRate * format.blockAlign)
      {
        return makeError(Error::Code::CorruptData, "WAV fmt chunk byte layout is inconsistent");
      }

      return format;
    }

    std::array<char, 4> readChunkId(std::span<std::byte const> bytes, std::size_t offset) noexcept
    {
      return {static_cast<char>(std::to_integer<unsigned char>(bytes[offset])),
              static_cast<char>(std::to_integer<unsigned char>(bytes[offset + 1])),
              static_cast<char>(std::to_integer<unsigned char>(bytes[offset + 2])),
              static_cast<char>(std::to_integer<unsigned char>(bytes[offset + 3]))};
    }

    Result<> applyChunk(ParsedWave& parsed, ChunkView const& chunk, bool& hasFormat, bool& hasData)
    {
      if (hasChunkId(chunk, "fmt "))
      {
        if (hasFormat)
        {
          return makeError(Error::Code::CorruptData, "WAV file has multiple fmt chunks");
        }

        auto formatResult = parseFormatChunk(chunk.bytes);

        if (!formatResult)
        {
          return std::unexpected{formatResult.error()};
        }

        parsed.format = *formatResult;
        hasFormat = true;
      }
      else if (hasChunkId(chunk, "data") && !chunk.bytes.empty())
      {
        if (hasData)
        {
          return makeError(Error::Code::CorruptData, "WAV file has multiple data chunks");
        }

        parsed.dataOffset = chunk.offset;
        parsed.data = chunk.bytes;
        hasData = true;
      }

      return {};
    }
  } // namespace

  Result<ParsedWave> parseWave(std::span<std::byte const> bytes, WaveParseExtent extent)
  {
    if (bytes.size() < kRiffHeaderSize)
    {
      return makeError(Error::Code::CorruptData, "WAV file is too small");
    }

    if (!matchesByteText(bytes.first(4), "RIFF"))
    {
      return makeError(Error::Code::NotSupported, "Unsupported WAV container");
    }

    if (!matchesByteText(bytes.subspan(8, 4), "WAVE"))
    {
      return makeError(Error::Code::CorruptData, "RIFF file is not a WAVE file");
    }

    auto const riffSize = readLe32(bytes, 4);

    if (riffSize < 4 || static_cast<std::uint64_t>(riffSize) + 8ULL > bytes.size())
    {
      return makeError(Error::Code::CorruptData, "WAV RIFF size exceeds file size");
    }

    auto const riffEnd = static_cast<std::size_t>(riffSize) + 8U;
    auto parsed = ParsedWave{};
    bool hasFormat = false;
    bool hasData = false;
    auto offset = kRiffHeaderSize;

    while (offset < riffEnd)
    {
      if (riffEnd - offset < kChunkHeaderSize)
      {
        return makeError(Error::Code::CorruptData, "WAV chunk header is truncated");
      }

      auto const chunkId = readChunkId(bytes, offset);
      auto const chunkSize = readLe32(bytes, offset + 4U);
      auto const payloadOffset = offset + kChunkHeaderSize;
      auto const payloadEnd = payloadOffset + static_cast<std::size_t>(chunkSize);
      auto const paddedEnd = payloadEnd + (chunkSize & 1U);

      if (payloadEnd < payloadOffset || paddedEnd < payloadEnd || paddedEnd > riffEnd)
      {
        return makeError(Error::Code::CorruptData, "WAV chunk size exceeds RIFF bounds");
      }

      auto chunk = ChunkView{.id = chunkId, .offset = payloadOffset, .bytes = bytes.subspan(payloadOffset, chunkSize)};
      parsed.chunks.push_back(chunk);

      auto applyResult = applyChunk(parsed, chunk, hasFormat, hasData);

      if (!applyResult)
      {
        return std::unexpected{applyResult.error()};
      }

      offset = paddedEnd;

      if (extent == WaveParseExtent::RequiredAudio && hasFormat && hasData)
      {
        break;
      }
    }

    if (!hasFormat)
    {
      return makeError(Error::Code::CorruptData, "WAV file has no fmt chunk");
    }

    if (!hasData || parsed.data.empty())
    {
      return makeError(Error::Code::CorruptData, "WAV file has no audio data");
    }

    if ((parsed.data.size() % parsed.format.blockAlign) != 0)
    {
      return makeError(Error::Code::CorruptData, "WAV data chunk is not frame-aligned");
    }

    return parsed;
  }

  bool hasChunkId(ChunkView const& chunk, std::string_view id) noexcept
  {
    if (id.size() != chunk.id.size())
    {
      return false;
    }

    for (std::size_t index = 0; index < chunk.id.size(); ++index)
    {
      if (chunk.id[index] != id[index])
      {
        return false;
      }
    }

    return true;
  }
} // namespace ao::media::wav
