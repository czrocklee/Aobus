// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "File.h"

#include "../detail/Decoder.h"
#include "Frame.h"
#include "id3v2/Layout.h"
#include "id3v2/Reader.h"
#include <ao/AudioCodec.h>
#include <ao/Error.h>
#include <ao/library/TrackBuilder.h>
#include <ao/tag/TagFile.h>
#include <ao/tag/detail/TagError.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <optional>
#include <string_view>

namespace ao::tag::mpeg
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
    constexpr std::size_t kApeV2SizeOffset = 12;
    constexpr std::size_t kApeV2FlagsOffset = 20;
    constexpr std::uint32_t kApeV2ContainsHeaderFlag = 0x80000000U;

    std::optional<std::size_t> id3v2TotalSize(void const* address, std::size_t fileSize) noexcept
    {
      if (fileSize < sizeof(id3v2::HeaderLayout) || std::memcmp(address, kId3v2Magic.data(), kId3v2Magic.size()) != 0)
      {
        return std::nullopt;
      }

      auto const* const id3v2Header = static_cast<id3v2::HeaderLayout const*>(address);
      std::size_t totalSize = sizeof(id3v2::HeaderLayout) + id3v2::decodeSize(id3v2Header->size);

      if (id3v2Header->majorVersion >= kId3v2MajorVersion4 && (id3v2Header->flags & kId3v2FooterFlag) != 0)
      {
        totalSize += kId3v2FooterSize;
      }

      if (totalSize > fileSize)
      {
        return std::nullopt;
      }

      return totalSize;
    }

    bool hasId3v1Tag(void const* address, std::size_t endOffset) noexcept
    {
      if (endOffset < kId3v1TagSize)
      {
        return false;
      }

      auto const* const end = static_cast<std::uint8_t const*>(address) + endOffset;
      return std::memcmp(end - kId3v1TagSize, kId3v1Magic.data(), kId3v1Magic.size()) == 0;
    }

    std::uint32_t readLittleEndianU32(std::uint8_t const* data) noexcept
    {
      return static_cast<std::uint32_t>(data[0]) | (static_cast<std::uint32_t>(data[1]) << 8U) |
             (static_cast<std::uint32_t>(data[2]) << 16U) | (static_cast<std::uint32_t>(data[3]) << 24U);
    }

    Result<std::size_t> trimTrailingTags(void const* address, std::size_t fileSize)
    {
      std::size_t payloadEnd = fileSize;

      if (hasId3v1Tag(address, payloadEnd))
      {
        payloadEnd -= kId3v1TagSize;
      }

      if (payloadEnd < kApeV2FooterSize)
      {
        return payloadEnd;
      }

      auto const* const apeFooter = static_cast<std::uint8_t const*>(address) + payloadEnd - kApeV2FooterSize;

      if (std::memcmp(apeFooter, kApeV2Magic.data(), kApeV2Magic.size()) != 0)
      {
        return payloadEnd;
      }

      auto const tagSize = static_cast<std::size_t>(readLittleEndianU32(apeFooter + kApeV2SizeOffset));

      if (tagSize < kApeV2FooterSize || tagSize > payloadEnd)
      {
        return makeError(Error::Code::CorruptData, "invalid apev2 trailing tag size");
      }

      auto totalTagSize = tagSize;
      auto const flags = readLittleEndianU32(apeFooter + kApeV2FlagsOffset);

      if ((flags & kApeV2ContainsHeaderFlag) != 0)
      {
        if (tagSize > payloadEnd - kApeV2FooterSize)
        {
          return makeError(Error::Code::CorruptData, "invalid apev2 trailing tag header size");
        }

        totalTagSize += kApeV2FooterSize;
        auto const* const apeHeader = static_cast<std::uint8_t const*>(address) + payloadEnd - totalTagSize;

        if (std::memcmp(apeHeader, kApeV2Magic.data(), kApeV2Magic.size()) != 0)
        {
          return makeError(Error::Code::CorruptData, "invalid apev2 trailing tag header");
        }
      }

      return payloadEnd - totalTagSize;
    }
  } // namespace

  std::chrono::milliseconds File::calculateDuration(FrameView const& frame, bool hasId3v1) const
  {
    constexpr std::uint32_t kMsPerSecond = 1000;
    constexpr std::uint32_t kBitsPerByte = 8;

    auto duration = std::chrono::milliseconds{0};

    // 1. Prefer Xing/Info header if present for accurate duration (especially VBR)
    if (auto optXing = frame.xingInfo(); optXing)
    {
      if (optXing->frames > 0 && frame.sampleRate() > 0)
      {
        duration = std::chrono::milliseconds{
          (static_cast<std::uint64_t>(optXing->frames) * frame.samplesPerFrame() * kMsPerSecond) / frame.sampleRate()};
      }
    }

    // 2. Fallback to estimation from file size if Xing/Info not present or failed
    if (duration == std::chrono::milliseconds{0} && frame.bitrate() > 0)
    {
      auto const* frameStart = static_cast<std::uint8_t const*>(frame.data());
      auto const* bufferEnd = static_cast<std::uint8_t const*>(address()) + size();

      if (hasId3v1)
      {
        bufferEnd -= kId3v1TagSize;
      }

      if (bufferEnd > frameStart)
      {
        std::uint64_t const actualAudioBytes = static_cast<std::uint64_t>(bufferEnd - frameStart);
        duration = std::chrono::milliseconds{(actualAudioBytes * kMsPerSecond * kBitsPerByte) / frame.bitrate()};
      }
    }

    return duration;
  }

  Result<library::TrackBuilder> File::loadTrackImpl() const
  {
    try
    {
      clearOwnedStrings();
      auto builder = library::TrackBuilder::makeEmpty();

      void const* audioStart = address();
      std::size_t audioSize = size();

      // 1. Try to parse ID3v2 tag at the beginning
      if (auto const optId3v2TotalSize = id3v2TotalSize(address(), size()); optId3v2TotalSize)
      {
        auto const* id3v2Header = static_cast<id3v2::HeaderLayout const*>(address());
        auto const id3v2BodySize = id3v2::decodeSize(id3v2Header->size);
        builder = id3v2::loadFrames(*this, *id3v2Header, id3v2Header + 1, id3v2BodySize);
        audioStart = static_cast<std::uint8_t const*>(audioStart) + *optId3v2TotalSize;
        audioSize -= *optId3v2TotalSize;
      }

      // 2. Check for ID3v1 tag at the end (128 bytes starting with "TAG")
      bool hasId3v1 = false;

      if (audioSize >= kId3v1TagSize && hasId3v1Tag(address(), size()))
      {
        audioSize -= kId3v1TagSize;
        hasId3v1 = true;
      }

      // 3. Locate first valid MPEG frame and extract audio properties
      if (auto optFrameView = locate(audioStart, audioSize); optFrameView)
      {
        auto bitrate = optFrameView->bitrate();

        builder.property()
          .sampleRate(SampleRate{optFrameView->sampleRate()})
          .bitrate(Bitrate{bitrate})
          .channels(Channels{optFrameView->channels()})
          .bitDepth(BitDepth{16})
          .codec(AudioCodec::Mp3);

        auto const duration = calculateDuration(*optFrameView, hasId3v1);
        builder.property().duration(duration);

        // If Xing/Info provides total bytes, we can refine the bitrate
        if (auto optXing = optFrameView->xingInfo();
            optXing && optXing->bytes > 0 && duration > std::chrono::milliseconds{0})
        {
          bitrate = bitrateFromBytes(static_cast<std::uint64_t>(optXing->bytes), duration);
          builder.property().bitrate(Bitrate{bitrate});
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
    void const* audioStart = address();
    std::size_t audioStartOffset = 0;

    if (auto const optId3v2TotalSize = id3v2TotalSize(address(), size()); optId3v2TotalSize)
    {
      audioStart = static_cast<std::uint8_t const*>(audioStart) + *optId3v2TotalSize;
      audioStartOffset = *optId3v2TotalSize;
    }

    auto const payloadEndResult = trimTrailingTags(address(), size());

    if (!payloadEndResult)
    {
      return std::unexpected{payloadEndResult.error()};
    }

    auto const payloadEnd = *payloadEndResult;

    if (payloadEnd <= audioStartOffset)
    {
      return makeError(Error::Code::CorruptData, "mpeg file has no audio payload");
    }

    if (auto const optFrameView = locate(audioStart, payloadEnd - audioStartOffset); optFrameView)
    {
      auto const offset = static_cast<std::size_t>(static_cast<std::uint8_t const*>(optFrameView->data()) -
                                                   static_cast<std::uint8_t const*>(address()));
      return payloadRange(offset, payloadEnd - offset);
    }

    return makeError(Error::Code::CorruptData, "mpeg file has no valid audio frame");
  }
} // namespace ao::tag::mpeg
