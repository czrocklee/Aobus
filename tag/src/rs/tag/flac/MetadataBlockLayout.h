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

#pragma once

#include <boost/endian/buffers.hpp>
#include <array>
#include <type_traits>
#include <cstdint>

namespace rs::tag::flac
{
  enum class MetadataBlockType : std::uint8_t
  {
    StreamInfo = 0,
    Padding = 1,
    Application = 2,
    SeekTable = 3,
    VorbisComment = 4,
    CueSheet = 5,
    Picture = 6,
    Invalid = 127
  };

  struct MetadataBlockLayout
  {
    using FixedSize = std::false_type;

    MetadataBlockType type : 7;
    bool isLastBlock : 1;
    boost::endian::big_uint24_buf_t size;
  };

  static_assert(sizeof(MetadataBlockLayout) == 4);
  static_assert(alignof(MetadataBlockLayout) == 1);
  static_assert(std::is_trivial_v<MetadataBlockLayout>);
  
}