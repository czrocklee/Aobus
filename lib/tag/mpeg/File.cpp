// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "Frame.h"
#include "id3v2/Layout.h"
#include "id3v2/Reader.h"
#include <rs/tag/mpeg/File.h>

#include <cstring>

namespace rs::tag::mpeg
{
  rs::core::TrackBuilder File::loadTrack() const
  {
    clearOwnedStrings();
    auto builder = rs::core::TrackBuilder::createNew();

    void const* audioStart = _mappedRegion.get_address();
    std::size_t audioSize = _mappedRegion.get_size();

    // 1. Try to parse ID3v2 tag at the beginning
    
    if (_mappedRegion.get_size() >= sizeof(id3v2::HeaderLayout) &&
        std::memcmp(_mappedRegion.get_address(), "ID3", 3) == 0)
    {
      auto const* id3v2Header = static_cast<id3v2::HeaderLayout const*>(_mappedRegion.get_address());
      std::size_t const id3v2BodySize = id3v2::decodeSize(id3v2Header->size);
      std::size_t id3v2TotalSize = sizeof(id3v2::HeaderLayout) + id3v2BodySize;

      // Handle ID3v2.4 footer (10 bytes if flag 0x10 is set)
      
      if (id3v2Header->majorVersion >= 4 && (id3v2Header->flags & 0x10))
      {
        id3v2TotalSize += 10;
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
    
    if (audioSize >= 128)
    {
      auto const* end = static_cast<std::uint8_t const*>(_mappedRegion.get_address()) + _mappedRegion.get_size();
      
      if (std::memcmp(end - 128, "TAG", 3) == 0)
      {
        audioSize -= 128;
        hasId3v1 = true;
      }
    }

    // 3. Locate first valid MPEG frame and extract audio properties
    
    if (auto frameView = locate(audioStart, audioSize))
    {
      auto bitrate = frameView->bitrate();
      auto durationMs = std::uint32_t{0};

      builder.property()
        .sampleRate(frameView->sampleRate())
        .bitrate(bitrate)
        .channels(frameView->channels())
        .bitDepth(16)   // MPEG audio is 16-bit
        .codecId(0x55); // MP3

      // 4. Calculate duration
      // Prefer Xing/Info header if present for accurate duration (especially VBR)
      
      if (auto xing = frameView->xingInfo())
      {
        if (xing->frames > 0 && frameView->sampleRate() > 0)
        {
          durationMs = static_cast<std::uint32_t>(
            (static_cast<std::uint64_t>(xing->frames) * frameView->samplesPerFrame() * 1000) / frameView->sampleRate());
          builder.property().durationMs(durationMs);

          // If Xing/Info provides total bytes, we can refine the bitrate
          
          if (xing->bytes > 0 && durationMs > 0)
          {
            bitrate = static_cast<std::uint32_t>((static_cast<std::uint64_t>(xing->bytes) * 8000) / durationMs);
            builder.property().bitrate(bitrate);
          }
        }
      }

      // Fallback to estimation from file size if Xing/Info not present or failed
      
      if (durationMs == 0 && bitrate > 0)
      {
        auto const* firstFramePtr = static_cast<std::uint8_t const*>(frameView->data());
        auto const* lastBytePtr =
          static_cast<std::uint8_t const*>(_mappedRegion.get_address()) + _mappedRegion.get_size();
        
        if (hasId3v1)
        {
          lastBytePtr -= 128;
        }

        if (lastBytePtr > firstFramePtr)
        {
          std::uint64_t const actualAudioBytes = lastBytePtr - firstFramePtr;
          builder.property().durationMs(static_cast<std::uint32_t>((actualAudioBytes * 8000) / bitrate));
        }
      }
    }

    return builder;
  }
} // namespace rs::tag::mpeg
