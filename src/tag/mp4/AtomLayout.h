// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <array>
#include <boost/endian/buffers.hpp>
#include <cstdint>
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
  static_assert(std::is_trivial_v<AtomLayout>);

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
  static_assert(std::is_trivial_v<DataAtomLayout>);

  struct TrknAtomLayout
  {
    using FixedSize = std::true_type;

    DataAtomLayout common;
    boost::endian::big_uint16_buf_t pad1;
    boost::endian::big_uint16_buf_t trackNumber;
    boost::endian::big_uint16_buf_t totalTracks;
    boost::endian::big_uint16_buf_t pad2;

    static constexpr char const* Type = "trkn";
  };

  static_assert(sizeof(TrknAtomLayout) == 32);
  static_assert(alignof(TrknAtomLayout) == 1);
  static_assert(std::is_trivial_v<TrknAtomLayout>);

  struct DiskAtomLayout
  {
    using FixedSize = std::true_type;

    DataAtomLayout common;
    boost::endian::big_uint16_buf_t pad1;
    boost::endian::big_uint16_buf_t discNumber;
    boost::endian::big_uint16_buf_t totalDiscs;

    static constexpr char const* Type = "disk";
  };

  static_assert(sizeof(DiskAtomLayout) == 30);
  static_assert(alignof(DiskAtomLayout) == 1);
  static_assert(std::is_trivial_v<DiskAtomLayout>);
}