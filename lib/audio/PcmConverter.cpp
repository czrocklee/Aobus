// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/audio/PcmConverter.h>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>

namespace ao::audio
{
  void PcmConverter::unpackS24(std::span<std::byte const> source,
                               std::span<std::int32_t> destination,
                               std::uint8_t shift) noexcept
  {
    auto const count = std::min(source.size() / 3, destination.size());

    static constexpr std::int32_t kS24SignBit = 0x800000;
    static constexpr std::uint32_t kS24SignExtensionMask = 0xFF000000U;

    for (std::size_t i = 0; i < count; ++i)
    {
      auto const offset = i * 3;

      // Manual unpack for little-endian S24
      auto bits = static_cast<std::uint32_t>(static_cast<std::uint8_t>(source[offset])) |
                  (static_cast<std::uint32_t>(static_cast<std::uint8_t>(source[offset + 1])) << 8U) |
                  (static_cast<std::uint32_t>(static_cast<std::uint8_t>(source[offset + 2])) << 16U);

      // Sign extension from 24 to 32 bits
      if ((bits & kS24SignBit) != 0)
      {
        bits |= kS24SignExtensionMask;
      }

      bits <<= shift;
      destination[i] = std::bit_cast<std::int32_t>(bits);
    }
  }
} // namespace ao::audio
