// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "File.h"
#include "Frame.h"
#include "id3v2/Layout.h"
#include "id3v2/Reader.h"
#include <cstring>

namespace ao::tag::mpeg
{
  namespace
  {
    constexpr std::uint8_t kCodecIdMp3 = 0x55;
  }

  std::uint32_t File::calculateDuration(FrameView const& frame, bool hasId3v1) const
  {
    constexpr std::uint32_t kMsPerSecond = 1000;
    constexpr std::uint32_t kBitsPerByte = 8;
    constexpr std::size_t kId3v1TagSize = 128;

    std::uint32_t durationMs = 0;

    // 1. Prefer Xing/Info header if present for accurate duration (especially VBR)
    if (auto xing = frame.xingInfo())
    {
      if (xing->frames > 0 && frame.sampleRate() > 0)
      {
        durationMs = static_cast<std::uint32_t>(
          (static_cast<std::uint64_t>(xing->frames) * frame.samplesPerFrame() * kMsPerSecond) / frame.sampleRate());
      }
    }

    // 2. Fallback to estimation from file size if Xing/Info not present or failed
    if (durationMs == 0 && frame.bitrate() > 0)
    {
      auto const* firstFramePtr = static_cast<std::uint8_t const*>(frame.data());
      auto const* lastBytePtr =
        static_cast<std::uint8_t const*>(_mappedRegion.get_address()) + _mappedRegion.get_size();

      if (hasId3v1)
      {
        lastBytePtr -= kId3v1TagSize;
      }

      if (lastBytePtr > firstFramePtr)
      {
        std::uint64_t const actualAudioBytes = lastBytePtr - firstFramePtr;
        durationMs = static_cast<std::uint32_t>((actualAudioBytes * kMsPerSecond * kBitsPerByte) / frame.bitrate());
      }
    }

    return durationMs;
  }

  library::TrackBuilder File::loadTrack() const
  {
    clearOwnedStrings();
    auto builder = library::TrackBuilder::createNew();

    void const* audioStart = _mappedRegion.get_address();
    std::size_t audioSize = _mappedRegion.get_size();

    // 1. Try to parse ID3v2 tag at the beginning

    if (_mappedRegion.get_size() >= sizeof(id3v2::HeaderLayout) &&
        std::memcmp(_mappedRegion.get_address(), "ID3", sizeof("ID3") - 1) == 0)
    {
      auto const* id3v2Header = static_cast<id3v2::HeaderLayout const*>(_mappedRegion.get_address());
      std::size_t const id3v2BodySize = id3v2::decodeSize(id3v2Header->size);
      std::size_t id3v2TotalSize = sizeof(id3v2::HeaderLayout) + id3v2BodySize;

      // Handle ID3v2.4 footer (10 bytes if flag 0x10 is set)
      constexpr std::size_t kId3v2FooterSize = 10;
      constexpr std::uint8_t kId3v2FooterFlag = 0x10;
      if (id3v2Header->majorVersion >= 4 && (id3v2Header->flags & kId3v2FooterFlag) != 0)
      {
        id3v2TotalSize += kId3v2FooterSize;
      }

      if (id3v2TotalSize <= _mappedRegion.get_size())
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
      auto const* end = static_cast<std::uint8_t const*>(_mappedRegion.get_address()) + _mappedRegion.get_size();

      if (std::memcmp(end - kId3v1TagSize, "TAG", sizeof("TAG") - 1) == 0)
      {
        audioSize -= kId3v1TagSize;
        hasId3v1 = true;
      }
    }

    // 3. Locate first valid MPEG frame and extract audio properties

    if (auto frameView = locate(audioStart, audioSize))
    {
      auto bitrate = frameView->bitrate();
      std::uint32_t durationMs = 0;

      builder.property()
        .sampleRate(frameView->sampleRate())
        .bitrate(bitrate)
        .channels(frameView->channels())
        .bitDepth(16)
        .codecId(kCodecIdMp3);

      durationMs = calculateDuration(*frameView, hasId3v1);
      builder.property().durationMs(durationMs);

      // If Xing/Info provides total bytes, we can refine the bitrate
      if (auto xing = frameView->xingInfo(); xing && xing->bytes > 0 && durationMs > 0)
      {
        constexpr std::uint32_t kMsPerSecond = 1000;
        constexpr std::uint32_t kBitsPerByte = 8;
        bitrate = static_cast<std::uint32_t>((static_cast<std::uint64_t>(xing->bytes) * kMsPerSecond * kBitsPerByte) /
                                             durationMs);
        builder.property().bitrate(bitrate);
      }
    }

    return builder;
  }
} // namespace ao::tag::mpeg
