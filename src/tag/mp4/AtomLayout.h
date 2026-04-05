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

  // mdhd (Media Header) - timescale and duration
  // Version 0 layout (most common):
  // bytes 0-3: length
  // bytes 4-7: "mdhd"
  // bytes 8-11: version (0) + flags (24 bits)
  // bytes 12-15: creation time
  // bytes 16-19: modification time
  // bytes 20-23: timescale (samples per second)
  // bytes 24-27: duration (in timescale units)
  struct MdhdAtomLayout
  {
    using FixedSize = std::true_type;

    AtomLayout common;
    boost::endian::big_uint32_buf_t versionAndFlags;
    boost::endian::big_uint32_buf_t creationTime;
    boost::endian::big_uint32_buf_t modificationTime;
    boost::endian::big_uint32_buf_t timescale;
    boost::endian::big_uint32_buf_t duration;

    static constexpr char const* Type = "mdhd";
  };

  static_assert(sizeof(MdhdAtomLayout) == 28);
  static_assert(alignof(MdhdAtomLayout) == 1);
  static_assert(std::is_trivial_v<MdhdAtomLayout>);

  // AudioSampleEntry for stsd - contains audio codec info
  // Full AudioSampleEntry with header is 28 bytes:
  // bytes 0-3: reserved (4)
  // bytes 4-5: data reference index (2)
  // bytes 6-7: reserved (2)
  // bytes 8-9: channel count (2)
  // bytes 10-11: sample size (2)
  // bytes 12-13: pre-defined + reserved (2)
  // bytes 14-15: reserved (2)
  // bytes 16-19: sample rate (4)
  struct AudioSampleEntryLayout
  {
    using FixedSize = std::true_type;

    std::array<char, 4> reserved1;
    boost::endian::big_uint16_buf_t dataReferenceIndex;
    std::array<char, 2> reserved2;
    boost::endian::big_uint16_buf_t channelCount;
    boost::endian::big_uint16_buf_t sampleSize;
    std::array<char, 4> reserved3;
    boost::endian::big_uint32_buf_t sampleRate;

    static constexpr char const* Type = "mp4a"; // or "ac3" etc
  };

  static_assert(sizeof(AudioSampleEntryLayout) == 20);
  static_assert(alignof(AudioSampleEntryLayout) == 1);
  static_assert(std::is_trivial_v<AudioSampleEntryLayout>);
}