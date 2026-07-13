// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "File.h"

#include "../detail/Content.h"
#include "../detail/Decoder.h"
#include <ao/AudioCodec.h>
#include <ao/Error.h>
#include <ao/PictureType.h>
#include <ao/media/flac/MetadataBlockLayout.h>
#include <ao/utility/ByteView.h>

#include <boost/endian/conversion.hpp>
#include <boost/endian/detail/order.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::media::file::flac
{
  using namespace media::flac;

  namespace
  {
    using TextSetter =
      detail::ContentBuilder::MetadataBuilder& (detail::ContentBuilder::MetadataBuilder::*)(std::string_view);
    using NumberSetter =
      detail::ContentBuilder::MetadataBuilder& (detail::ContentBuilder::MetadataBuilder::*)(std::uint16_t);

    template<TextSetter Setter>
    void handleText(detail::ContentBuilder& builder, std::string_view value)
    {
      (builder.metadata().*Setter)(value);
    }

    [[maybe_unused]] void handleEnsembleFallback(detail::ContentBuilder& builder, std::string_view value)
    {
      if (builder.metadata().ensemble().empty())
      {
        builder.metadata().ensemble(value);
      }
    }

    [[maybe_unused]] void handleSoloistFallback(detail::ContentBuilder& builder, std::string_view value)
    {
      if (builder.metadata().soloist().empty())
      {
        builder.metadata().soloist(value);
      }
    }

    template<NumberSetter Setter>
    void handleNumber(detail::ContentBuilder& builder, std::string_view value)
    {
      if (auto const optParsed = decodeUint16(value); optParsed)
      {
        (builder.metadata().*Setter)(*optParsed);
      }
    }

    template<NumberSetter PrimarySetter, NumberSetter SecondarySetter>
    void handleSlashNumber(detail::ContentBuilder& builder, std::string_view value)
    {
      auto const pair = parseSlashPair(value);

      if (pair.optPrimary)
      {
        (builder.metadata().*PrimarySetter)(*pair.optPrimary);
      }

      if (pair.optSecondary)
      {
        (builder.metadata().*SecondarySetter)(*pair.optSecondary);
      }
    }

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4267)
#else
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#endif
#include "media/file/flac/VorbisCommentDispatch.h"
#ifdef _MSC_VER
#pragma warning(pop)
#else
#pragma GCC diagnostic pop
#endif

    std::optional<std::uint32_t> readU32(std::span<std::byte const> bytes,
                                         std::size_t& offset,
                                         boost::endian::order order) noexcept
    {
      if (offset > bytes.size() || sizeof(std::uint32_t) > bytes.size() - offset)
      {
        return std::nullopt;
      }

      std::uint32_t value = 0;
      std::memcpy(&value, bytes.data() + offset, sizeof(value));
      offset += sizeof(value);

      if (order == boost::endian::order::big)
      {
        boost::endian::big_to_native_inplace(value);
      }
      else
      {
        boost::endian::little_to_native_inplace(value);
      }

      return value;
    }

    std::optional<std::span<std::byte const>> readSized(std::span<std::byte const> bytes,
                                                        std::size_t& offset,
                                                        boost::endian::order order) noexcept
    {
      auto const optLength = readU32(bytes, offset, order);

      if (!optLength || offset > bytes.size() || *optLength > bytes.size() - offset)
      {
        return std::nullopt;
      }

      auto const value = bytes.subspan(offset, *optLength);
      offset += *optLength;
      return value;
    }

    std::optional<std::vector<std::string_view>> parseComments(std::span<std::byte const> payload)
    {
      std::size_t offset = 0;

      if (!readSized(payload, offset, boost::endian::order::little))
      {
        return std::nullopt;
      }

      auto const optCount = readU32(payload, offset, boost::endian::order::little);

      if (!optCount)
      {
        return std::nullopt;
      }

      auto comments = std::vector<std::string_view>{};

      for (std::uint32_t index = 0; index < *optCount; ++index)
      {
        auto const optComment = readSized(payload, offset, boost::endian::order::little);

        if (!optComment)
        {
          return std::nullopt;
        }

        comments.push_back(utility::bytes::stringView(*optComment));
      }

      if (offset != payload.size())
      {
        return std::nullopt;
      }

      return comments;
    }

    struct ParsedPicture final
    {
      PictureType type = PictureType::Other;
      std::span<std::byte const> bytes;
    };

    std::optional<ParsedPicture> parsePicture(std::span<std::byte const> payload) noexcept
    {
      constexpr std::size_t kPictureScalarCount = 4;
      std::size_t offset = 0;
      auto const optRawType = readU32(payload, offset, boost::endian::order::big);

      if (!optRawType || !readSized(payload, offset, boost::endian::order::big) ||
          !readSized(payload, offset, boost::endian::order::big))
      {
        return std::nullopt;
      }

      for (std::size_t index = 0; index < kPictureScalarCount; ++index)
      {
        if (!readU32(payload, offset, boost::endian::order::big))
        {
          return std::nullopt;
        }
      }

      auto const optBytes = readSized(payload, offset, boost::endian::order::big);

      if (!optBytes || offset != payload.size())
      {
        return std::nullopt;
      }

      auto const type = *optRawType <= static_cast<std::uint32_t>(PictureType::PublisherLogo)
                          ? static_cast<PictureType>(*optRawType)
                          : PictureType::Other;
      return ParsedPicture{.type = type, .bytes = *optBytes};
    }

    std::uint32_t sampleRate(StreamInfoLayout const& layout) noexcept
    {
      constexpr std::uint64_t kShift = 44;
      constexpr std::uint64_t kMask = 0xFFFFF;
      return static_cast<std::uint32_t>((layout.packedFields.value() >> kShift) & kMask);
    }

    std::uint8_t channels(StreamInfoLayout const& layout) noexcept
    {
      constexpr std::uint64_t kShift = 41;
      constexpr std::uint64_t kMask = 0x07;
      return static_cast<std::uint8_t>(((layout.packedFields.value() >> kShift) & kMask) + 1);
    }

    std::uint8_t bitDepth(StreamInfoLayout const& layout) noexcept
    {
      constexpr std::uint64_t kShift = 36;
      constexpr std::uint64_t kMask = 0x1F;
      return static_cast<std::uint8_t>(((layout.packedFields.value() >> kShift) & kMask) + 1);
    }

    std::uint64_t totalSamples(StreamInfoLayout const& layout) noexcept
    {
      constexpr std::uint64_t kMask = 0xFFFFFFFFF;
      return layout.packedFields.value() & kMask;
    }

    void appendStreamInfo(detail::ContentBuilder& builder, std::span<std::byte const> payload, std::size_t fileSize)
    {
      auto const* const layout = utility::layout::view<StreamInfoLayout>(payload);
      auto const rate = sampleRate(*layout);
      builder.property()
        .sampleRate(SampleRate{rate})
        .channels(Channels{channels(*layout)})
        .bitDepth(BitDepth{bitDepth(*layout)})
        .codec(AudioCodec::Flac);

      if (auto const samples = totalSamples(*layout); rate > 0 && samples > 0)
      {
        auto const duration = std::chrono::milliseconds{(samples * std::chrono::milliseconds::period::den) / rate};

        if (duration > std::chrono::milliseconds{0})
        {
          builder.property().duration(duration).bitrate(Bitrate{bitrateFromBytes(fileSize, duration)});
        }
      }
    }

    void appendVorbisComments(detail::ContentBuilder& builder, std::span<std::byte const> payload)
    {
      auto const optComments = parseComments(payload);

      if (!optComments)
      {
        return;
      }

      for (auto const comment : *optComments)
      {
        auto const equalsOffset = comment.find('=');

        if (equalsOffset == std::string_view::npos)
        {
          continue;
        }

        auto const key = comment.substr(0, equalsOffset);
        auto const value = comment.substr(equalsOffset + 1);

        if (auto const* const entry = FlacVorbisDispatchTable::lookupVorbisField(key.data(), key.size());
            entry != nullptr)
        {
          entry->handler(builder, value);
        }
      }
    }

    void appendPicture(detail::ContentBuilder& builder, std::span<std::byte const> payload)
    {
      if (auto const optPicture = parsePicture(payload); optPicture)
      {
        builder.coverArt().add(optPicture->type, optPicture->bytes);
      }
    }
  } // namespace

  Result<File::Index> File::parseIndex() const
  {
    auto const fileBytes = bytes();

    if (fileBytes.size() < 4 || std::memcmp(fileBytes.data(), "fLaC", 4) != 0)
    {
      return makeError(Error::Code::CorruptData, "unrecognized flac file content");
    }

    auto result = Index{};
    std::size_t offset = 4;
    bool isFirst = true;
    bool sawStreamInfo = false;

    while (true)
    {
      if (offset > fileBytes.size() || sizeof(MetadataBlockLayout) > fileBytes.size() - offset)
      {
        return makeError(Error::Code::CorruptData, "invalid flac metadata block header");
      }

      auto const* const header = utility::layout::view<MetadataBlockLayout>(fileBytes.subspan(offset));
      auto const payloadSize = static_cast<std::size_t>(header->size.value());
      auto const payloadOffset = offset + sizeof(MetadataBlockLayout);

      if (payloadOffset > fileBytes.size() || payloadSize > fileBytes.size() - payloadOffset)
      {
        return makeError(Error::Code::CorruptData, "invalid flac metadata block boundary");
      }

      if (isFirst && header->type != MetadataBlockType::StreamInfo)
      {
        return makeError(Error::Code::CorruptData, "first flac metadata block is not StreamInfo");
      }

      if (header->type == MetadataBlockType::StreamInfo)
      {
        if (sawStreamInfo || payloadSize != StreamInfoLayout::kSize)
        {
          return makeError(Error::Code::CorruptData, "invalid flac StreamInfo block");
        }

        sawStreamInfo = true;
      }

      result.blocks.push_back(
        BlockView{.type = header->type, .payload = fileBytes.subspan(payloadOffset, payloadSize)});
      offset = payloadOffset + payloadSize;
      isFirst = false;

      if (header->isLastBlock)
      {
        break;
      }
    }

    if (offset == fileBytes.size())
    {
      return makeError(Error::Code::CorruptData, "flac file has no audio payload");
    }

    result.payload = payloadRange(offset, fileBytes.size() - offset);
    return result;
  }

  Result<File::Index> const& File::index() const
  {
    if (!_optIndexResult)
    {
      _optIndexResult.emplace(parseIndex());
    }

    return *_optIndexResult;
  }

  Result<detail::Content> File::readContent() const
  {
    auto const& indexResult = index();

    if (!indexResult)
    {
      return std::unexpected{indexResult.error()};
    }

    auto builder = detail::ContentBuilder::makeEmpty();

    for (auto const& block : indexResult->blocks)
    {
      switch (block.type)
      {
        case MetadataBlockType::StreamInfo: appendStreamInfo(builder, block.payload, bytes().size()); break;

        case MetadataBlockType::VorbisComment: appendVorbisComments(builder, block.payload); break;

        case MetadataBlockType::Picture: appendPicture(builder, block.payload); break;

        default: break;
      }
    }

    return std::move(builder).finish();
  }

  Result<PayloadView> File::audioPayload() const
  {
    auto const& indexResult = index();

    if (!indexResult)
    {
      return std::unexpected{indexResult.error()};
    }

    return indexResult->payload;
  }
} // namespace ao::media::file::flac
