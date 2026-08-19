// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/utility/ByteView.h>

#include <boost/endian/buffers.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace ao::media::ogg
{
  inline constexpr auto kCapturePattern = std::to_array({'O', 'g', 'g', 'S'});

  inline constexpr std::uint8_t kSupportedVersion = 0;

  // Header type flags carried by every page.
  inline constexpr std::uint8_t kContinuedPacketFlag = 0x01;
  inline constexpr std::uint8_t kBeginOfStreamFlag = 0x02;
  inline constexpr std::uint8_t kEndOfStreamFlag = 0x04;

  // A lacing value below this terminates the packet it belongs to; a value of
  // exactly this continues the packet into the next segment.
  inline constexpr std::uint8_t kContinuationLacingValue = 255;

  struct PageHeaderLayout final
  {
    static constexpr std::size_t kSize = 27;

    using FixedSize = std::true_type;

    std::array<char, 4> capturePattern;
    std::uint8_t version;
    std::uint8_t headerType;
    boost::endian::little_int64_buf_t granulePosition;
    boost::endian::little_uint32_buf_t serialNumber;
    boost::endian::little_uint32_buf_t pageSequence;
    boost::endian::little_uint32_buf_t checksum;
    std::uint8_t segmentCount;
  };

  static_assert(sizeof(PageHeaderLayout) == PageHeaderLayout::kSize, "PageHeaderLayout should be 27 bytes");
  static_assert(alignof(PageHeaderLayout) == 1);
  static_assert(utility::layout::kIsBinaryLayoutType<PageHeaderLayout>);
} // namespace ao::media::ogg
