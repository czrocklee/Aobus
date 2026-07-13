// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "File.h"

#include "../detail/Content.h"
#include "../detail/Decoder.h"
#include "../detail/Reader.h"
#include "Frame.h"
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
    constexpr std::size_t kApeV2SizeOffset = 12;
    constexpr std::size_t kApeV2FlagsOffset = 20;
    constexpr std::uint32_t kApeV2ContainsHeaderFlag = 0x80000000U;
    constexpr std::uint8_t kSyncSafeHighBit = 0x80U;

    std::optional<std::size_t> id3v2TotalSize(std::span<std::byte const> bytes) noexcept
    {
      if (bytes.size() < sizeof(id3v2::HeaderLayout) ||
          std::memcmp(bytes.data(), kId3v2Magic.data(), kId3v2Magic.size()) != 0)
      {
        return std::nullopt;
      }

      auto const* const header = utility::layout::view<id3v2::HeaderLayout>(bytes);

      for (auto const byte : header->size.data)
      {
        if ((byte & kSyncSafeHighBit) != 0)
        {
          return std::nullopt;
        }
      }

      std::size_t totalSize = sizeof(id3v2::HeaderLayout) + id3v2::decodeSize(header->size);

      if (header->majorVersion >= kId3v2MajorVersion4 && (header->flags & kId3v2FooterFlag) != 0)
      {
        totalSize += kId3v2FooterSize;
      }

      return totalSize <= bytes.size() ? std::optional{totalSize} : std::nullopt;
    }

    bool hasId3v1Tag(std::span<std::byte const> bytes, std::size_t endOffset) noexcept
    {
      return endOffset >= kId3v1TagSize &&
             std::memcmp(bytes.data() + endOffset - kId3v1TagSize, kId3v1Magic.data(), kId3v1Magic.size()) == 0;
    }

    std::uint32_t readLittleEndianU32(std::byte const* data) noexcept
    {
      return std::to_integer<std::uint32_t>(data[0]) | (std::to_integer<std::uint32_t>(data[1]) << 8U) |
             (std::to_integer<std::uint32_t>(data[2]) << 16U) | (std::to_integer<std::uint32_t>(data[3]) << 24U);
    }

    Result<std::size_t> trimTrailingTags(std::span<std::byte const> bytes)
    {
      std::size_t payloadEnd = bytes.size();

      if (hasId3v1Tag(bytes, payloadEnd))
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

      auto const tagSize = static_cast<std::size_t>(readLittleEndianU32(footer + kApeV2SizeOffset));

      if (tagSize < kApeV2FooterSize || tagSize > payloadEnd)
      {
        return makeError(Error::Code::CorruptData, "invalid apev2 trailing tag size");
      }

      auto totalTagSize = tagSize;
      auto const flags = readLittleEndianU32(footer + kApeV2FlagsOffset);

      if ((flags & kApeV2ContainsHeaderFlag) != 0)
      {
        if (tagSize > payloadEnd - kApeV2FooterSize)
        {
          return makeError(Error::Code::CorruptData, "invalid apev2 trailing tag header size");
        }

        totalTagSize += kApeV2FooterSize;
        auto const* const header = bytes.data() + payloadEnd - totalTagSize;

        if (std::memcmp(header, kApeV2Magic.data(), kApeV2Magic.size()) != 0)
        {
          return makeError(Error::Code::CorruptData, "invalid apev2 trailing tag header");
        }
      }

      return payloadEnd - totalTagSize;
    }
  } // namespace

  struct File::Index final
  {
    PayloadView payload;
    FrameView firstFrame{nullptr, 0};
    id3v2::HeaderLayout const* id3Header = nullptr;
    std::span<std::byte const> id3Body;
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
      std::size_t audioStartOffset = 0;
      auto result = Index{};

      if (auto const optId3Size = id3v2TotalSize(fileBytes); optId3Size)
      {
        auto const* const header = utility::layout::view<id3v2::HeaderLayout>(fileBytes);
        auto const bodySize = id3v2::decodeSize(header->size);
        result.id3Header = header;
        result.id3Body = fileBytes.subspan(sizeof(id3v2::HeaderLayout), bodySize);
        audioStartOffset = *optId3Size;
      }

      auto const payloadEndResult = trimTrailingTags(fileBytes);

      if (!payloadEndResult)
      {
        return std::unexpected{payloadEndResult.error()};
      }

      auto const payloadEnd = *payloadEndResult;

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
      result.payload = payloadRange(frameOffset, payloadEnd - frameOffset);
      result.firstFrame = *optFrame;
      return result;
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
    auto const& indexResult = index();

    if (!indexResult)
    {
      return std::unexpected{indexResult.error()};
    }

    auto builder = detail::ContentBuilder::makeEmpty();

    if (indexResult->id3Header != nullptr)
    {
      if (auto optTagBuilder = id3v2::readFrames(*indexResult->id3Header, indexResult->id3Body); optTagBuilder)
      {
        builder = std::move(*optTagBuilder);
      }
    }

    auto const& frame = indexResult->firstFrame;
    auto bitrate = frame.bitrate();
    builder.property()
      .sampleRate(SampleRate{frame.sampleRate()})
      .bitrate(Bitrate{bitrate})
      .channels(Channels{frame.channels()})
      .bitDepth(BitDepth{16})
      .codec(AudioCodec::Mp3);

    auto const duration = calculateDuration(frame, indexResult->payload.bytes.size());
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
    auto const& indexResult = index();

    if (!indexResult)
    {
      return std::unexpected{indexResult.error()};
    }

    return indexResult->payload;
  }
} // namespace ao::media::file::mpeg
