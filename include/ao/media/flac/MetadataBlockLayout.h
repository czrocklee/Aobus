// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <array>
#include <boost/endian/buffers.hpp>
#include <cstdint>
#include <type_traits>

namespace ao::media::flac
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

  struct StreamInfoLayout
  {
    static constexpr std::size_t kSize = 34;

    using FixedSize = std::true_type;
    boost::endian::big_uint16_buf_t minBlockSize;
    boost::endian::big_uint16_buf_t maxBlockSize;
    boost::endian::big_uint24_buf_t minFrameSize;
    boost::endian::big_uint24_buf_t maxFrameSize;
    // 64 bits: sampleRate(20) + channels-1(3) + bits-1(5) + totalSamples(36)
    boost::endian::big_uint64_buf_t packedFields;
    std::array<std::uint8_t, 16> md5;
  };

  static_assert(sizeof(StreamInfoLayout) == StreamInfoLayout::kSize, "StreamInfoDataLayout should be 34 bytes");
  static_assert(alignof(StreamInfoLayout) == 1);
  static_assert(std::is_trivial_v<StreamInfoLayout>);
} // namespace ao::media::flac
