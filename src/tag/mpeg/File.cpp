// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "../Decoder.h"
#include "id3v2/Layout.h"
#include "id3v2/Reader.h"
#include <rs/tag/mpeg/File.h>

#include <charconv>
#include <map>

namespace rs::tag::mpeg
{
  Metadata File::loadMetadata() const
  {
    if (_mappedRegion.get_size() < sizeof(id3v2::HeaderLayout) ||
        std::memcmp(_mappedRegion.get_address(), "ID3", 3) != 0)
    {
      return {};
    }

    auto const* id3v2Header = static_cast<id3v2::HeaderLayout const*>(_mappedRegion.get_address());
    std::size_t size = id3v2::decodeSize(id3v2Header->size);

    if (size + sizeof(id3v2::HeaderLayout) > _mappedRegion.get_size())
    {
      return {};
    }

    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    return id3v2::loadFrames(*id3v2Header, id3v2Header + 1, size);
    // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  }

  void File::saveMetadata(Metadata const&) {}
}