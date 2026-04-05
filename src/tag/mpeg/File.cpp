// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "id3v2/Layout.h"
#include "id3v2/Reader.h"
#include "Frame.h"
#include <rs/tag/mpeg/File.h>

#include <cstring>

namespace rs::tag::mpeg
{
  ParsedTrack File::loadTrack() const
  {
    ParsedTrack parsed;

    // First, parse ID3v2 tag for metadata
    if (_mappedRegion.get_size() < sizeof(id3v2::HeaderLayout) ||
        std::memcmp(_mappedRegion.get_address(), "ID3", 3) != 0)
    {
      return {};
    }

    auto const* id3v2Header = static_cast<id3v2::HeaderLayout const*>(_mappedRegion.get_address());
    std::size_t id3v2Size = id3v2::decodeSize(id3v2Header->size);

    if (id3v2Size + sizeof(id3v2::HeaderLayout) > _mappedRegion.get_size()) { return {}; }

    // Parse ID3v2 frames for metadata
    parsed = id3v2::loadFrames(*id3v2Header, id3v2Header + 1, id3v2Size);

    // Now try to extract audio properties from MPEG frames
    // Audio data starts after the ID3v2 tag
    auto const* audioStart = static_cast<std::uint8_t const*>(_mappedRegion.get_address()) + sizeof(id3v2::HeaderLayout) + id3v2Size;
    auto const audioSize = _mappedRegion.get_size() - sizeof(id3v2::HeaderLayout) - id3v2Size;

    if (auto frameView = locate(audioStart, audioSize))
    {
      parsed.record.property.sampleRate = frameView->sampleRate();
      parsed.record.property.bitrate = frameView->bitrate();
      parsed.record.property.channels = frameView->channels();
      // MPEG audio is always 16-bit PCM
      parsed.record.property.bitDepth = 16;
      // codecId for MPEG Layer III is typically 0x55 (MP3)
      parsed.record.property.codecId = 0x55;
    }

    return parsed;
  }
} // namespace rs::tag::mpeg
