// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <array>
#include <boost/endian/buffers.hpp>
#include <cstdint>
#include <type_traits>

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