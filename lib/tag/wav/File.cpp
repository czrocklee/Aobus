// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "File.h"

#include "../detail/Decoder.h"
#include "../mpeg/id3v2/Layout.h"
#include "../mpeg/id3v2/Reader.h"
#include <ao/AudioCodec.h>
#include <ao/AudioScalars.h>
#include <ao/Error.h>
#include <ao/library/TrackBuilder.h>
#include <ao/media/wav/Riff.h>
#include <ao/tag/TagFile.h>
#include <ao/tag/detail/TagError.h>
#include <ao/utility/ByteView.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace ao::tag::wav
{
  namespace
  {
    constexpr std::uint16_t kDecimalBase = 10;

    std::string_view trimTrailingNulls(std::string_view text) noexcept
    {
      while (!text.empty() && text.back() == '\0')
      {
        text.remove_suffix(1);
      }

      return text;
    }

    std::uint32_t readLe32(std::span<std::byte const> bytes, std::size_t offset) noexcept
    {
      return static_cast<std::uint32_t>(std::to_integer<std::uint32_t>(bytes[offset])) |
             (static_cast<std::uint32_t>(std::to_integer<std::uint32_t>(bytes[offset + 1])) << 8U) |
             (static_cast<std::uint32_t>(std::to_integer<std::uint32_t>(bytes[offset + 2])) << 16U) |
             (static_cast<std::uint32_t>(std::to_integer<std::uint32_t>(bytes[offset + 3])) << 24U);
    }

    bool equalsId(std::span<std::byte const> bytes, std::string_view id) noexcept
    {
      if (bytes.size() != id.size())
      {
        return false;
      }

      for (std::size_t index = 0; index < bytes.size(); ++index)
      {
        if (std::to_integer<unsigned char>(bytes[index]) != static_cast<unsigned char>(id[index]))
        {
          return false;
        }
      }

      return true;
    }

    std::uint16_t yearFromText(std::string_view text) noexcept
    {
      if (text.size() < 4)
      {
        return 0;
      }

      std::uint16_t year = 0;

      for (std::size_t index = 0; index < 4; ++index)
      {
        auto const ch = text[index];

        if (ch < '0' || ch > '9')
        {
          return 0;
        }

        year = static_cast<std::uint16_t>((year * kDecimalBase) + static_cast<std::uint16_t>(ch - '0'));
      }

      return year;
    }

    void applyInfoField(library::TrackBuilder& builder, std::string_view id, std::string_view value)
    {
      if (value.empty())
      {
        return;
      }

      if (id == "INAM")
      {
        builder.metadata().title(value);
      }
      else if (id == "IART")
      {
        builder.metadata().artist(value);
      }
      else if (id == "IPRD")
      {
        builder.metadata().album(value);
      }
      else if (id == "IGNR")
      {
        builder.metadata().genre(value);
      }
      else if (id == "ICRD")
      {
        if (auto const year = yearFromText(value); year != 0)
        {
          builder.metadata().year(year);
        }
      }
    }

    void readInfoList(library::TrackBuilder& builder, media::wav::ChunkView const& chunk)
    {
      if (chunk.bytes.size() < 4 || !equalsId(chunk.bytes.first(4), "INFO"))
      {
        return;
      }

      std::size_t offset = 4;

      while (offset < chunk.bytes.size())
      {
        if (chunk.bytes.size() - offset < 8)
        {
          return;
        }

        auto const fieldId = utility::bytes::stringView(chunk.bytes.subspan(offset, 4));
        auto const fieldSize = readLe32(chunk.bytes, offset + 4U);
        auto const valueOffset = offset + 8U;
        auto const valueEnd = valueOffset + static_cast<std::size_t>(fieldSize);
        auto const paddedEnd = valueEnd + (fieldSize & 1U);

        if (valueEnd < valueOffset || paddedEnd < valueEnd || paddedEnd > chunk.bytes.size())
        {
          return;
        }

        auto const value = trimTrailingNulls(utility::bytes::stringView(chunk.bytes.subspan(valueOffset, fieldSize)));
        applyInfoField(builder, fieldId, value);
        offset = paddedEnd;
      }
    }

    void mergeTextMetadata(library::TrackBuilder& target, library::TrackBuilder const& source)
    {
      auto const& metadata = source.metadata();

      if (!metadata.title().empty())
      {
        target.metadata().title(metadata.title());
      }

      if (!metadata.artist().empty())
      {
        target.metadata().artist(metadata.artist());
      }

      if (!metadata.album().empty())
      {
        target.metadata().album(metadata.album());
      }

      if (!metadata.albumArtist().empty())
      {
        target.metadata().albumArtist(metadata.albumArtist());
      }

      if (!metadata.composer().empty())
      {
        target.metadata().composer(metadata.composer());
      }

      if (!metadata.conductor().empty())
      {
        target.metadata().conductor(metadata.conductor());
      }

      if (!metadata.ensemble().empty())
      {
        target.metadata().ensemble(metadata.ensemble());
      }

      if (!metadata.genre().empty())
      {
        target.metadata().genre(metadata.genre());
      }

      if (!metadata.work().empty())
      {
        target.metadata().work(metadata.work());
      }

      if (!metadata.movement().empty())
      {
        target.metadata().movement(metadata.movement());
      }

      if (!metadata.soloist().empty())
      {
        target.metadata().soloist(metadata.soloist());
      }

      if (metadata.year() != 0)
      {
        target.metadata().year(metadata.year());
      }

      if (metadata.trackNumber() != 0)
      {
        target.metadata().trackNumber(metadata.trackNumber());
      }

      if (metadata.trackTotal() != 0)
      {
        target.metadata().trackTotal(metadata.trackTotal());
      }

      if (metadata.discNumber() != 0)
      {
        target.metadata().discNumber(metadata.discNumber());
      }

      if (metadata.discTotal() != 0)
      {
        target.metadata().discTotal(metadata.discTotal());
      }

      if (metadata.movementNumber() != 0)
      {
        target.metadata().movementNumber(metadata.movementNumber());
      }

      if (metadata.movementTotal() != 0)
      {
        target.metadata().movementTotal(metadata.movementTotal());
      }
    }

    void readId3Chunk(library::TrackBuilder& builder, TagFile const& owner, media::wav::ChunkView const& chunk)
    {
      if (chunk.bytes.size() < mpeg::id3v2::HeaderLayout::kSize)
      {
        return;
      }

      auto const* header = utility::bytes::tryLayout<mpeg::id3v2::HeaderLayout>(chunk.bytes);

      if (header == nullptr || header->id != std::array<char, 3>{'I', 'D', '3'})
      {
        return;
      }

      auto const tagSize = mpeg::id3v2::decodeSize(header->size);
      auto const frameOffset = mpeg::id3v2::HeaderLayout::kSize;

      if (tagSize > chunk.bytes.size() - frameOffset)
      {
        return;
      }

      auto id3Builder = mpeg::id3v2::loadFrames(owner, *header, chunk.bytes.data() + frameOffset, tagSize);
      mergeTextMetadata(builder, id3Builder);
    }
  } // namespace

  Result<library::TrackBuilder> File::loadTrackImpl() const
  {
    try
    {
      clearOwnedStrings();

      auto parsedResult = media::wav::parseWave(utility::bytes::view(address(), size()));

      if (!parsedResult)
      {
        return std::unexpected{parsedResult.error()};
      }

      auto const& parsed = *parsedResult;
      auto builder = library::TrackBuilder::makeEmpty();
      auto const totalFrames = parsed.data.size() / parsed.format.blockAlign;
      auto const duration = std::chrono::milliseconds{
        (static_cast<std::uint64_t>(totalFrames) * std::chrono::milliseconds::period::den) / parsed.format.sampleRate};

      builder.property()
        .sampleRate(SampleRate{parsed.format.sampleRate})
        .channels(Channels{static_cast<std::uint8_t>(parsed.format.channels)})
        .bitDepth(BitDepth{static_cast<std::uint8_t>(parsed.format.validBitsPerSample)})
        .codec(AudioCodec::Wav);

      if (duration > std::chrono::milliseconds{0})
      {
        builder.property().duration(duration).bitrate(Bitrate{bitrateFromBytes(size(), duration)});
      }

      for (auto const& chunk : parsed.chunks)
      {
        if (media::wav::hasChunkId(chunk, "LIST"))
        {
          readInfoList(builder, chunk);
        }
        else if (media::wav::hasChunkId(chunk, "id3 ") || media::wav::hasChunkId(chunk, "ID3 "))
        {
          readId3Chunk(builder, *this, chunk);
        }
      }

      return builder;
    }
    catch (detail::TagException const& ex)
    {
      return std::unexpected{ex.error()};
    }
  }

  Result<AudioPayload> File::audioPayloadImpl() const
  {
    auto parsedResult = media::wav::parseWave(utility::bytes::view(address(), size()));

    if (!parsedResult)
    {
      return std::unexpected{parsedResult.error()};
    }

    return payloadRange(parsedResult->dataOffset, parsedResult->data.size());
  }
} // namespace ao::tag::wav
