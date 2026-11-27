// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "Layout.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace ao::media::file::mpeg::id3v2
{
  std::optional<std::size_t> tryDecodeSize(EncodedSize size) noexcept
  {
    constexpr std::uint8_t kHighBit = 0x80U;

    if (std::ranges::any_of(size.data, [](std::uint8_t byte) { return (byte & kHighBit) != 0; }))
    {
      return std::nullopt;
    }

    static constexpr std::size_t kByteShift3 = 21;
    static constexpr std::size_t kByteShift2 = 14;
    static constexpr std::size_t kByteShift1 = 7;

    return (static_cast<std::size_t>(size.data[0]) << kByteShift3) |
           (static_cast<std::size_t>(size.data[1]) << kByteShift2) |
           (static_cast<std::size_t>(size.data[2]) << kByteShift1) | static_cast<std::size_t>(size.data.back());
  }
} // namespace ao::media::file::mpeg::id3v2
