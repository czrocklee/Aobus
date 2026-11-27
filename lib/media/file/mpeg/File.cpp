// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "File.h"

#include "../detail/Content.h"
#include "../detail/Decoder.h"
#include "../detail/Reader.h"
#include "ApeTag.h"
#include "Frame.h"
#include "FrameLayout.h"
#include "id3v2/Layout.h"
#include "id3v2/Reader.h"
#include <ao/AudioCodec.h>
#include <ao/Error.h>
#include <ao/media/file/File.h>
#include <ao/utility/ByteView.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

namespace ao::media::file::mpeg
{
  namespace
  {
    constexpr std::string_view kId3v2Magic = "ID3";
    constexpr std::string_view kId3v1Magic = "TAG";
    constexpr std::string_view kApeV2Magic = "APETAGEX";
    constexpr std::size_t kId3v1TagSize = 128;
    constexpr std::size_t kId3v2FooterSize = 10;
    constexpr std::uint8_t kId3v2FooterFlag = 0x10;
    constexpr std::uint8_t kId3v2MajorVersion4 = 4;
    constexpr std::size_t kApeV2FooterSize = 32;

    struct LeadingTagView final
    {
      id3v2::HeaderLayout const* header;
      std::span<std::byte const> body;
      std::size_t endOffset;
    };

    std::optional<LeadingTagView> findLeadingTag(std::span<std::byte const> bytes) noexcept
    {
      if (bytes.size() < sizeof(id3v2::HeaderLayout) ||
          std::memcmp(bytes.data(), kId3v2Magic.data(), kId3v2Magic.size()) != 0)
      {
        return std::nullopt;
      }

      auto const* const header = utility::layout::view<id3v2::HeaderLayout>(bytes);

      auto const optBodySize = id3v2::tryDecodeSize(header->size);

      if (!optBodySize)
      {
        return std::nullopt;
      }

      auto totalSize = id3v2::HeaderLayout::kSize + *optBodySize;

      if (header->majorVersion >= kId3v2MajorVersion4 && (header->flags & kId3v2FooterFlag) != 0)
      {
        totalSize += kId3v2FooterSize;
      }

      if (totalSize > bytes.size())
      {
        return std::nullopt;
      }

      return LeadingTagView{
        .header = header, .body = bytes.subspan(id3v2::HeaderLayout::kSize, *optBodySize), .endOffset = totalSize};
    }

    bool hasId3v1Tag(std::span<std::byte const> bytes, std::size_t endOffset) noexcept
    {
      return endOffset >= kId3v1TagSize &&
             std::memcmp(bytes.data() + endOffset - kId3v1TagSize, kId3v1Magic.data(), kId3v1Magic.size()) == 0;
    }

    bool hasFrameChainPayloadAt(std::span<std::byte const> bytes, FrameView const& firstFrame, std::size_t offset)
    {
      auto const firstOffset =
        static_cast<std::size_t>(static_cast<std::byte const*>(firstFrame.data()) - bytes.data());
      auto frame = FrameView{firstFrame.data(), bytes.size() - firstOffset, firstFrame.length()};
      bool overlaps = false;

      while (true)
      {
        auto const frameOffset = static_cast<std::size_t>(static_cast<std::byte const*>(frame.data()) - bytes.data());
        auto const length = frame.length();

        if (length < sizeof(FrameLayout) || length > frame.size())
        {
          return false;
        }

        if (frameOffset < offset && offset - frameOffset < length)
        {
          overlaps = true;
        }

        auto const remaining = bytes.subspan(frameOffset + length);

        // Match locate()'s short terminal-byte tolerance, but account for the
        // whole chain: tag bytes must not merely complete the overlapping frame.
        if (remaining.size() < sizeof(FrameLayout))
        {
          return overlaps;
        }

        auto next = FrameView{remaining.data(), remaining.size()};

        if (!next.isValid())
        {
          return false;
        }

        auto const& currentLayout = frame.layout();
        auto const& nextLayout = next.layout();

        if (currentLayout.versionId() != nextLayout.versionId() || currentLayout.layer() != nextLayout.layer() ||
            currentLayout.samplingRateIndex() != nextLayout.samplingRateIndex())
        {
          return false;
        }

        if (nextLayout.bitrateIndex() == 0)
        {
          if (currentLayout.bitrateIndex() != 0)
          {
            return false;
          }

          std::size_t const paddingSlot = currentLayout.layer() == LayerDescription::LayerI ? 4 : 1;
          auto const baseLength = length - (currentLayout.paddingBit() != 0 ? paddingSlot : 0);
          auto const nextLength = baseLength + (nextLayout.paddingBit() != 0 ? paddingSlot : 0);
          next = FrameView{remaining.data(), remaining.size(), nextLength};
        }

        frame = next;
      }
    }

    Result<std::size_t> trailingTagPayloadEnd(std::span<std::byte const> bytes, bool trimId3v1)
    {
      std::size_t payloadEnd = bytes.size();

      if (trimId3v1 && hasId3v1Tag(bytes, payloadEnd))
      {
        payloadEnd -= kId3v1TagSize;
      }

      if (payloadEnd < kApeV2FooterSize)
      {
        return payloadEnd;
      }

      auto const* const footer = bytes.data() + payloadEnd - kApeV2FooterSize;

      if (std::memcmp(footer, kApeV2Magic.data(), kApeV2Magic.size()) != 0)
      {
        return payloadEnd;
      }

      return readApeTagStart(bytes.first(payloadEnd));
    }

    Result<std::size_t> trimTrailingTags(std::span<std::byte const> bytes, std::size_t audioStartOffset)
    {
      auto payloadEndRes = trailingTagPayloadEnd(bytes, true);

      if (!hasId3v1Tag(bytes, bytes.size()) || bytes.size() - kId3v1TagSize < audioStartOffset)
      {
        return payloadEndRes;
      }

      auto optFrame = std::optional<FrameView>{};

      // Keep the established audio start: MPEG-looking tag values must not replace it.
      if (payloadEndRes && *payloadEndRes > audioStartOffset)
      {
        auto const candidate = bytes.subspan(audioStartOffset, *payloadEndRes - audioStartOffset);
        optFrame = locate(candidate.data(), candidate.size());
      }

      if (!optFrame)
      {
        auto const untrimmed = bytes.subspan(audioStartOffset);
        optFrame = locate(untrimmed.data(), untrimmed.size());
      }

      if (optFrame)
      {
        // Keep independently recognized APE bytes outside the frame-chain
        // witness, even when TAG at physical EOF - 128 lies in preceding audio.
        auto const retainedEndRes = trailingTagPayloadEnd(bytes, false);
        auto const markerOffset = bytes.size() - kId3v1TagSize;
        auto const firstOffset =
          static_cast<std::size_t>(static_cast<std::byte const*>(optFrame->data()) - bytes.data());

        if (retainedEndRes && firstOffset < *retainedEndRes && markerOffset < *retainedEndRes &&
            hasFrameChainPayloadAt(bytes.first(*retainedEndRes), *optFrame, markerOffset))
        {
          return retainedEndRes;
        }
      }

      return payloadEndRes;
    }
  } // namespace

  struct File::Index final
  {
    PayloadView payload;
    FrameView firstFrame{nullptr, 0};
    std::optional<LeadingTagView> optTag;
  };

  struct File::CachedIndex final
  {
    std::optional<Result<Index>> optResult;
  };

  File::File(std::span<std::byte const> bytes)
    : detail::Reader{bytes}
  {
  }

  File::~File() = default;

  Result<File::Index> const& File::index() const
  {
    if (!_cachedIndexPtr)
    {
      _cachedIndexPtr = std::make_unique<CachedIndex>();
    }

    if (_cachedIndexPtr->optResult)
    {
      return *_cachedIndexPtr->optResult;
    }

    auto parse = [&] -> Result<Index>
    {
      auto const fileBytes = bytes();
      auto const optTag = findLeadingTag(fileBytes);
      auto const audioStartOffset = optTag ? optTag->endOffset : std::size_t{0};
      auto const payloadEndRes = trimTrailingTags(fileBytes, audioStartOffset);

      if (!payloadEndRes)
      {
        return std::unexpected{payloadEndRes.error()};
      }

      auto const payloadEnd = *payloadEndRes;

      if (payloadEnd <= audioStartOffset)
      {
        return makeError(Error::Code::CorruptData, "mpeg file has no audio payload");
      }

      auto const searchBytes = fileBytes.subspan(audioStartOffset, payloadEnd - audioStartOffset);
      auto const optFrame = locate(searchBytes.data(), searchBytes.size());

      if (!optFrame)
      {
        return makeError(Error::Code::CorruptData, "mpeg file has no complete audio frame");
      }

      auto const frameOffset =
        static_cast<std::size_t>(static_cast<std::byte const*>(optFrame->data()) - fileBytes.data());
      return Index{
        .payload = payloadRange(frameOffset, payloadEnd - frameOffset), .firstFrame = *optFrame, .optTag = optTag};
    };

    _cachedIndexPtr->optResult.emplace(parse());
    return *_cachedIndexPtr->optResult;
  }

  std::chrono::milliseconds File::calculateDuration(FrameView const& frame, std::size_t payloadSize)
  {
    constexpr std::uint32_t kMsPerSecond = 1000;
    constexpr std::uint32_t kBitsPerByte = 8;
    auto duration = std::chrono::milliseconds{0};

    if (auto const optXing = frame.xingInfo(); optXing && optXing->frames > 0 && frame.sampleRate() > 0)
    {
      duration = std::chrono::milliseconds{
        (static_cast<std::uint64_t>(optXing->frames) * frame.samplesPerFrame() * kMsPerSecond) / frame.sampleRate()};
    }

    if (duration == std::chrono::milliseconds{0} && frame.bitrate() > 0)
    {
      auto const audioBytes = static_cast<std::uint64_t>(payloadSize);
      duration = std::chrono::milliseconds{(audioBytes * kMsPerSecond * kBitsPerByte) / frame.bitrate()};
    }

    return duration;
  }

  Result<detail::Content> File::readContent() const
  {
    auto const& indexRes = index();

    if (!indexRes)
    {
      return std::unexpected{indexRes.error()};
    }

    auto builder = detail::ContentBuilder::makeEmpty();

    if (auto const& optTag = indexRes->optTag; optTag)
    {
      if (auto optTagBuilder = id3v2::readFrames(*optTag->header, optTag->body); optTagBuilder)
      {
        builder = std::move(*optTagBuilder);
      }
    }

    auto const& frame = indexRes->firstFrame;
    auto bitrate = frame.bitrate();
    builder.property()
      .sampleRate(SampleRate{frame.sampleRate()})
      .bitrate(Bitrate{bitrate})
      .channels(Channels{frame.channels()})
      .bitDepth(BitDepth{16})
      .codec(AudioCodec::Mp3);

    auto const duration = calculateDuration(frame, indexRes->payload.bytes.size());
    builder.property().duration(duration);

    if (auto const optXing = frame.xingInfo(); optXing && optXing->bytes > 0 && duration > std::chrono::milliseconds{0})
    {
      bitrate = bitrateFromBytes(static_cast<std::uint64_t>(optXing->bytes), duration);
      builder.property().bitrate(Bitrate{bitrate});
    }

    return std::move(builder).finish();
  }

  Result<PayloadView> File::audioPayload() const
  {
    auto const& indexRes = index();

    if (!indexRes)
    {
      return std::unexpected{indexRes.error()};
    }

    return indexRes->payload;
  }
} // namespace ao::media::file::mpeg
