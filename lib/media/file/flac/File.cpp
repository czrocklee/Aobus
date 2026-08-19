// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "File.h"

#include "../detail/Content.h"
#include "../detail/Decoder.h"
#include "../detail/PictureBlock.h"
#include "../detail/VorbisComment.h"
#include <ao/AudioCodec.h>
#include <ao/Error.h>
#include <ao/media/flac/MetadataBlockLayout.h>
#include <ao/utility/ByteView.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <optional>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

namespace ao::media::file::flac
{
  using namespace media::flac;

  namespace
  {
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
      auto const optComments = detail::parseVorbisComments(payload, detail::VorbisCommentTrailing::Rejected);

      if (!optComments)
      {
        return;
      }

      for (auto const comment : *optComments)
      {
        if (auto const optField = detail::splitVorbisComment(comment); optField)
        {
          std::ignore = detail::applyVorbisComment(builder, *optField);
        }
      }
    }

    void appendPicture(detail::ContentBuilder& builder, std::span<std::byte const> payload)
    {
      if (auto const optBlock = detail::parsePictureBlock(payload); optBlock)
      {
        builder.coverArt().add(optBlock->type, optBlock->bytes);
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
