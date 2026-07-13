// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "File.h"

#include "../detail/Content.h"
#include "../detail/Decoder.h"
#include "../mpeg/id3v2/Layout.h"
#include "../mpeg/id3v2/Reader.h"
#include <ao/AudioCodec.h>
#include <ao/AudioScalars.h>
#include <ao/Error.h>
#include <ao/media/file/File.h>
#include <ao/media/wav/Riff.h>
#include <ao/utility/ByteView.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::media::file::wav
{
  namespace
  {
    constexpr std::uint16_t kDecimalBase = 10;
    constexpr std::uint8_t kSyncSafeHighBit = 0x80U;

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

    void applyInfoField(detail::ContentBuilder& builder, std::string_view id, std::string_view value)
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

    void readInfoList(detail::ContentBuilder& builder, media::wav::ChunkView const& chunk)
    {
      if (chunk.bytes.size() < 4 || !equalsId(chunk.bytes.first(4), "INFO"))
      {
        return;
      }

      std::size_t offset = 4;
      auto fields = std::vector<std::pair<std::string_view, std::string_view>>{};

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
        fields.emplace_back(fieldId, value);
        offset = paddedEnd;
      }

      for (auto const& [fieldId, value] : fields)
      {
        applyInfoField(builder, fieldId, value);
      }
    }

    void copyTextMetadata(detail::ContentBuilder& target, detail::ContentBuilder const& source)
    {
      auto const& metadata = source.metadata();
      auto copyText = [&target](std::string_view value, auto setter)
      {
        if (!value.empty())
        {
          setter(target.metadata(), target.own(std::string{value}));
        }
      };

      copyText(metadata.title(), [](auto& builder, auto value) { builder.title(value); });
      copyText(metadata.artist(), [](auto& builder, auto value) { builder.artist(value); });
      copyText(metadata.album(), [](auto& builder, auto value) { builder.album(value); });
      copyText(metadata.albumArtist(), [](auto& builder, auto value) { builder.albumArtist(value); });
      copyText(metadata.composer(), [](auto& builder, auto value) { builder.composer(value); });
      copyText(metadata.conductor(), [](auto& builder, auto value) { builder.conductor(value); });
      copyText(metadata.ensemble(), [](auto& builder, auto value) { builder.ensemble(value); });
      copyText(metadata.genre(), [](auto& builder, auto value) { builder.genre(value); });
      copyText(metadata.work(), [](auto& builder, auto value) { builder.work(value); });
      copyText(metadata.movement(), [](auto& builder, auto value) { builder.movement(value); });
      copyText(metadata.soloist(), [](auto& builder, auto value) { builder.soloist(value); });

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

    bool hasValidSyncSafeSize(mpeg::id3v2::EncodedSize const& size) noexcept
    {
      return std::ranges::all_of(size.data, [](std::uint8_t byte) { return (byte & kSyncSafeHighBit) == 0; });
    }

    void readId3Chunk(detail::ContentBuilder& builder, media::wav::ChunkView const& chunk)
    {
      if (chunk.bytes.size() < mpeg::id3v2::HeaderLayout::kSize)
      {
        return;
      }

      auto const* const header = utility::bytes::tryLayout<mpeg::id3v2::HeaderLayout>(chunk.bytes);

      if (header == nullptr || header->id != std::array<char, 3>{'I', 'D', '3'} || !hasValidSyncSafeSize(header->size))
      {
        return;
      }

      auto const tagSize = mpeg::id3v2::decodeSize(header->size);
      auto const frameOffset = mpeg::id3v2::HeaderLayout::kSize;

      if (tagSize > chunk.bytes.size() - frameOffset)
      {
        return;
      }

      auto const frames = chunk.bytes.subspan(frameOffset, tagSize);

      if (auto optId3Builder = mpeg::id3v2::readFrames(*header, frames); optId3Builder)
      {
        copyTextMetadata(builder, *optId3Builder);
      }
    }
  } // namespace

  Result<media::wav::ParsedWave> const& File::parsed() const
  {
    if (!_optParsedResult)
    {
      _optParsedResult.emplace(media::wav::parseWave(bytes()));
    }

    return *_optParsedResult;
  }

  Result<detail::Content> File::readContent() const
  {
    auto const& parsedResult = parsed();

    if (!parsedResult)
    {
      return std::unexpected{parsedResult.error()};
    }

    auto const& wave = *parsedResult;
    auto builder = detail::ContentBuilder::makeEmpty();
    auto const totalFrames = wave.data.size() / wave.format.blockAlign;
    auto const duration = std::chrono::milliseconds{
      (static_cast<std::uint64_t>(totalFrames) * std::chrono::milliseconds::period::den) / wave.format.sampleRate};

    builder.property()
      .sampleRate(SampleRate{wave.format.sampleRate})
      .channels(Channels{static_cast<std::uint8_t>(wave.format.channels)})
      .bitDepth(BitDepth{static_cast<std::uint8_t>(wave.format.validBitsPerSample)})
      .codec(AudioCodec::Wav);

    if (duration > std::chrono::milliseconds{0})
    {
      builder.property().duration(duration).bitrate(Bitrate{bitrateFromBytes(bytes().size(), duration)});
    }

    for (auto const& chunk : wave.chunks)
    {
      if (media::wav::hasChunkId(chunk, "LIST"))
      {
        readInfoList(builder, chunk);
      }
      else if (media::wav::hasChunkId(chunk, "id3 ") || media::wav::hasChunkId(chunk, "ID3 "))
      {
        readId3Chunk(builder, chunk);
      }
    }

    return std::move(builder).finish();
  }

  Result<PayloadView> File::audioPayload() const
  {
    auto const& parsedResult = parsed();

    if (!parsedResult)
    {
      return std::unexpected{parsedResult.error()};
    }

    return payloadRange(parsedResult->dataOffset, parsedResult->data.size());
  }
} // namespace ao::media::file::wav
