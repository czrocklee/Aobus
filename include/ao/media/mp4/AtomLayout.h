// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/utility/ByteView.h>

#include <boost/endian/buffers.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace ao::media::mp4
{
  constexpr std::size_t kAtomTypeSize = 4;
  constexpr std::size_t kAtomMagicSize = 4;

  struct AtomLayout
  {
    using FixedSize = std::false_type;

    boost::endian::big_uint32_buf_t length;
    std::array<char, kAtomTypeSize> type;
  };

  static_assert(sizeof(AtomLayout) == 8);
  static_assert(alignof(AtomLayout) == 1);
  static_assert(utility::layout::kIsBinaryLayoutType<AtomLayout>);

  struct DataAtomLayout
  {
    using FixedSize = std::false_type;

    enum class Type : std::uint8_t
    {
      Text = 1,
      Binary = 0
    };

    AtomLayout common;
    boost::endian::big_uint32_buf_t dataLength;
    std::array<char, kAtomMagicSize> magic;
    boost::endian::big_uint32_buf_t type;
    boost::endian::big_uint32_buf_t reserved;
  };

  static_assert(sizeof(DataAtomLayout) == 24);
  static_assert(alignof(DataAtomLayout) == 1);
  static_assert(utility::layout::kIsBinaryLayoutType<DataAtomLayout>);

  struct TrknAtomLayout
  {
    static constexpr std::size_t kByteCount = 32;
    using FixedSize = std::true_type;

    DataAtomLayout common;
    boost::endian::big_uint16_buf_t pad1;
    boost::endian::big_uint16_buf_t trackNumber;
    boost::endian::big_uint16_buf_t trackTotal;
    boost::endian::big_uint16_buf_t pad2;

    static constexpr char const* kType = "trkn";
  };

  static_assert(sizeof(TrknAtomLayout) == TrknAtomLayout::kByteCount);
  static_assert(alignof(TrknAtomLayout) == 1);
  static_assert(utility::layout::kIsBinaryLayoutType<TrknAtomLayout>);

  struct DiskAtomLayout
  {
    static constexpr std::size_t kByteCount = 30;
    using FixedSize = std::false_type;

    DataAtomLayout common;
    boost::endian::big_uint16_buf_t pad1;
    boost::endian::big_uint16_buf_t discNumber;
    boost::endian::big_uint16_buf_t discTotal;

    static constexpr char const* kType = "disk";
  };

  static_assert(sizeof(DiskAtomLayout) == DiskAtomLayout::kByteCount);
  static_assert(alignof(DiskAtomLayout) == 1);
  static_assert(utility::layout::kIsBinaryLayoutType<DiskAtomLayout>);

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
    static constexpr std::size_t kByteCount = 28;
    using FixedSize = std::false_type;

    AtomLayout common;
    boost::endian::big_uint32_buf_t versionAndFlags;
    boost::endian::big_uint32_buf_t creationTime;
    boost::endian::big_uint32_buf_t modificationTime;
    boost::endian::big_uint32_buf_t timescale;
    boost::endian::big_uint32_buf_t duration;

    static constexpr char const* kType = "mdhd";
  };

  static_assert(sizeof(MdhdAtomLayout) == MdhdAtomLayout::kByteCount);
  static_assert(alignof(MdhdAtomLayout) == 1);
  static_assert(utility::layout::kIsBinaryLayoutType<MdhdAtomLayout>);

  // stsd (Sample Description Box)
  // Fixed header: length + type + version/flags + entryCount (16 bytes)
  // Followed by entryCount sample entries
  struct StsdAtomLayout
  {
    static constexpr std::size_t kByteCount = 16;
    using FixedSize = std::false_type;

    AtomLayout common;
    boost::endian::big_uint32_buf_t versionAndFlags;
    boost::endian::big_uint32_buf_t entryCount;
  };

  static_assert(sizeof(StsdAtomLayout) == StsdAtomLayout::kByteCount);
  static_assert(alignof(StsdAtomLayout) == 1);
  static_assert(utility::layout::kIsBinaryLayoutType<StsdAtomLayout>);

  // stts (Time To Sample Box)
  // Fixed header: length + type + version/flags + entryCount (16 bytes)
  // Followed by entryCount entries of 8 bytes each
  struct SttsAtomLayout
  {
    static constexpr std::size_t kByteCount = 16;
    using FixedSize = std::false_type;

    AtomLayout common;
    boost::endian::big_uint32_buf_t versionAndFlags;
    boost::endian::big_uint32_buf_t entryCount;

    struct Entry
    {
      boost::endian::big_uint32_buf_t sampleCount;
      boost::endian::big_uint32_buf_t sampleDelta;
    };
  };

  static_assert(sizeof(SttsAtomLayout) == SttsAtomLayout::kByteCount);
  static_assert(alignof(SttsAtomLayout) == 1);
  static_assert(utility::layout::kIsBinaryLayoutType<SttsAtomLayout>);

  // stsz (Sample Size Box)  // Fixed header: length + type + version/flags + sampleSize + sampleCount (20 bytes)
  // Followed by sampleCount entries of 4 bytes each (when sampleSize == 0)
  struct StszAtomLayout
  {
    static constexpr std::size_t kByteCount = 20;
    using FixedSize = std::false_type;

    AtomLayout common;
    boost::endian::big_uint32_buf_t versionAndFlags;
    boost::endian::big_uint32_buf_t sampleSize;
    boost::endian::big_uint32_buf_t sampleCount;

    struct Entry
    {
      boost::endian::big_uint32_buf_t size;
    };
  };

  static_assert(sizeof(StszAtomLayout) == StszAtomLayout::kByteCount);
  static_assert(alignof(StszAtomLayout) == 1);
  static_assert(utility::layout::kIsBinaryLayoutType<StszAtomLayout>);

  // stsc (Sample To Chunk Box)
  // Fixed header: length + type + version/flags + entryCount (16 bytes)
  // Followed by entryCount entries of 12 bytes each
  struct StscAtomLayout
  {
    static constexpr std::size_t kByteCount = 16;
    using FixedSize = std::false_type;

    AtomLayout common;
    boost::endian::big_uint32_buf_t versionAndFlags;
    boost::endian::big_uint32_buf_t entryCount;

    struct Entry
    {
      boost::endian::big_uint32_buf_t firstChunk;
      boost::endian::big_uint32_buf_t samplesPerChunk;
      boost::endian::big_uint32_buf_t sampleDescIndex;
    };
  };

  static_assert(sizeof(StscAtomLayout) == StscAtomLayout::kByteCount);
  static_assert(alignof(StscAtomLayout) == 1);
  static_assert(utility::layout::kIsBinaryLayoutType<StscAtomLayout>);

  // stco (Chunk Offset Box - 32-bit)
  // Fixed header: length + type + version/flags + entryCount (16 bytes)
  // Followed by entryCount entries of 4 bytes each
  struct StcoAtomLayout
  {
    static constexpr std::size_t kByteCount = 16;
    using FixedSize = std::false_type;

    AtomLayout common;
    boost::endian::big_uint32_buf_t versionAndFlags;
    boost::endian::big_uint32_buf_t entryCount;

    struct Entry
    {
      boost::endian::big_uint32_buf_t chunkOffset;
    };
  };

  static_assert(sizeof(StcoAtomLayout) == StcoAtomLayout::kByteCount);
  static_assert(alignof(StcoAtomLayout) == 1);
  static_assert(utility::layout::kIsBinaryLayoutType<StcoAtomLayout>);

  // co64 (Chunk Offset Box - 64-bit)
  // Fixed header: length + type + version/flags + entryCount (16 bytes)
  // Followed by entryCount entries of 8 bytes each
  struct Co64AtomLayout
  {
    static constexpr std::size_t kByteCount = 16;
    using FixedSize = std::false_type;

    AtomLayout common;
    boost::endian::big_uint32_buf_t versionAndFlags;
    boost::endian::big_uint32_buf_t entryCount;

    struct Entry
    {
      boost::endian::big_uint64_buf_t chunkOffset;
    };
  };

  static_assert(sizeof(Co64AtomLayout) == Co64AtomLayout::kByteCount);
  static_assert(alignof(Co64AtomLayout) == 1);
  static_assert(utility::layout::kIsBinaryLayoutType<Co64AtomLayout>);

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
    static constexpr std::size_t kByteCount = 36;
    static constexpr std::size_t kReserved1Size = 6;
    using FixedSize = std::true_type;

    AtomLayout common;
    std::array<char, kReserved1Size> reserved1;
    boost::endian::big_uint16_buf_t dataReferenceIndex;
    std::array<boost::endian::big_uint16_buf_t, 4> reserved2;
    boost::endian::big_uint16_buf_t channelCount;
    boost::endian::big_uint16_buf_t sampleSize;
    boost::endian::big_uint16_buf_t preDefined;
    boost::endian::big_uint16_buf_t reserved3;
    boost::endian::big_uint32_buf_t sampleRate;

    static constexpr char const* kType = "mp4a";
  };

  static_assert(sizeof(AudioSampleEntryLayout) == AudioSampleEntryLayout::kByteCount);
  static_assert(alignof(AudioSampleEntryLayout) == 1);
  static_assert(utility::layout::kIsBinaryLayoutType<AudioSampleEntryLayout>);
} // namespace ao::media::mp4
