/*
 * Copyright (C) <year> <name of author>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of  MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Lesser General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <rs/tag/mpeg/File.h>
#include "id3v2/Reader.h"
#include "id3v2/Layout.h"
#include "../Decoder.h"
#include <map>
#include <charconv>

namespace rs::tag::mpeg
{
  namespace
  {
    

  }

  const Metadata File::loadMetadata() const
  {
    if (_mappedRegion.get_size() < sizeof(id3v2::HeaderLayout) || std::memcmp(_mappedRegion.get_address(), "ID3", 3) != 0)
    {
      return {};
    }

    const auto* id3v2Header = static_cast<const id3v2::HeaderLayout*>(_mappedRegion.get_address());
    std::size_t size = id3v2::decodeSize(id3v2Header->size);

    if (size + sizeof(id3v2::HeaderLayout) > _mappedRegion.get_size())
    {
      return {};
    }

    return id3v2::loadFrames(*id3v2Header, id3v2Header + 1, size);
  }

  void File::saveMetadata(const Metadata& metadata) {}
}