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

namespace rs::tag::mp4
{
  struct AtomLayout
  {
    using FixedSize = std::false_type;

    boost::endian::big_uint32_buf_t length;
    std::array<char, 4> type;
  };

  static_assert(sizeof(AtomLayout) == 8);
  static_assert(alignof(AtomLayout) == 1);
  static_assert(std::is_pod_v<AtomLayout>);

  struct DataAtomLayout 
  {
    using FixedSize = std::false_type;

    enum class Type : std::uint32_t 
    {
      Text = 1,
      Binary = 0
    };

    AtomLayout common;
    boost::endian::big_uint32_buf_t dataLength;
    std::array<char, 4> magic;
    boost::endian::big_uint32_buf_t type;
    boost::endian::big_uint32_buf_t reserved;
  };
  
  static_assert(sizeof(DataAtomLayout) == 24);
  static_assert(alignof(DataAtomLayout) == 1);
  static_assert(std::is_pod_v<DataAtomLayout>);

  struct TrknAtomLayout
  {
    using FixedSize = std::true_type;

    DataAtomLayout common;
    boost::endian::big_uint16_buf_t pad1;
    boost::endian::big_uint16_buf_t trackNumber;
    boost::endian::big_uint16_buf_t totalTracks;
    boost::endian::big_uint16_buf_t pad2;

    static constexpr const char* Type = "trkn";
  };

  static_assert(sizeof(TrknAtomLayout) == 32);
  static_assert(alignof(TrknAtomLayout) == 1);
  static_assert(std::is_pod_v<TrknAtomLayout>);

  struct DiskAtomLayout
  {
    using FixedSize = std::true_type;

    DataAtomLayout common;
    boost::endian::big_uint16_buf_t pad1;
    boost::endian::big_uint16_buf_t discNumber;
    boost::endian::big_uint16_buf_t totalDiscs;

    static constexpr const char* Type = "disk";
  };

  static_assert(sizeof(DiskAtomLayout) == 30);
  static_assert(alignof(DiskAtomLayout) == 1);
  static_assert(std::is_pod_v<DiskAtomLayout>);
}