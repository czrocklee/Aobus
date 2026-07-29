// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "Layout.h"

#include <cstddef>
#include <cstdint>

namespace ao::media::file::mpeg::id3v2
{
  std::size_t decodeSize(EncodedSize size)
  {
    static constexpr std::uint8_t kSyncSafeMask = 0x7F;
    static constexpr std::size_t kByteShift3 = 21;
    static constexpr std::size_t kByteShift2 = 14;
    static constexpr std::size_t kByteShift1 = 7;

    return (static_cast<std::size_t>(size.data[0] & kSyncSafeMask) << kByteShift3) |
           (static_cast<std::size_t>(size.data[1] & kSyncSafeMask) << kByteShift2) |
           (static_cast<std::size_t>(size.data[2] & kSyncSafeMask) << kByteShift1) |
           (static_cast<std::size_t>(size.data.back() & kSyncSafeMask));
  }
} // namespace ao::media::file::mpeg::id3v2
