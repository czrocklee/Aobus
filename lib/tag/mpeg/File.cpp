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
#include <ao/tag/detail/TagError.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <expected>
#include <string_view>

namespace ao::tag::mpeg
{
  std::chrono::milliseconds File::calculateDuration(FrameView const& frame, bool hasId3v1) const
  {
    constexpr std::uint32_t kMsPerSecond = 1000;
    constexpr std::uint32_t kBitsPerByte = 8;
    constexpr std::size_t kId3v1TagSize = 128;

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
      auto builder = library::TrackBuilder::createNew();

      void const* audioStart = address();
      std::size_t audioSize = size();

      // 1. Try to parse ID3v2 tag at the beginning
      static constexpr std::string_view kId3v2Magic = "ID3";

      if (size() >= sizeof(id3v2::HeaderLayout) && std::memcmp(address(), kId3v2Magic.data(), kId3v2Magic.size()) == 0)
      {
        auto const* id3v2Header = static_cast<id3v2::HeaderLayout const*>(address());
        std::size_t const id3v2BodySize = id3v2::decodeSize(id3v2Header->size);
        std::size_t id3v2TotalSize = sizeof(id3v2::HeaderLayout) + id3v2BodySize;

        // Handle ID3v2.4 footer (10 bytes if flag 0x10 is set)
        constexpr std::size_t kId3v2FooterSize = 10;
        constexpr std::uint8_t kId3v2FooterFlag = 0x10;
        constexpr std::uint8_t kId3v2MajorVersion4 = 4;

        if (id3v2Header->majorVersion >= kId3v2MajorVersion4 && (id3v2Header->flags & kId3v2FooterFlag) != 0)
        {
          id3v2TotalSize += kId3v2FooterSize;
        }

        if (id3v2TotalSize <= size())
        {
          builder = id3v2::loadFrames(*this, *id3v2Header, id3v2Header + 1, id3v2BodySize);
          audioStart = static_cast<std::uint8_t const*>(audioStart) + id3v2TotalSize;
          audioSize -= id3v2TotalSize;
        }
      }

      // 2. Check for ID3v1 tag at the end (128 bytes starting with "TAG")
      bool hasId3v1 = false;
      constexpr std::size_t kId3v1TagSize = 128;

      if (audioSize >= kId3v1TagSize)
      {
        auto const* end = static_cast<std::uint8_t const*>(address()) + size();

        static constexpr std::string_view kId3v1Magic = "TAG";

        if (std::memcmp(end - kId3v1TagSize, kId3v1Magic.data(), kId3v1Magic.size()) == 0)
        {
          audioSize -= kId3v1TagSize;
          hasId3v1 = true;
        }
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
} // namespace ao::tag::mpeg
