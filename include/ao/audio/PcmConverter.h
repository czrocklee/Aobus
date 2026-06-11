// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

namespace ao::audio
{
  /**
   * @brief High-performance PCM format conversion utilities.
   */
  class PcmConverter final
  {
  public:
    /**
     * @brief Pads samples from a lower bit depth to a higher bit depth with correct alignment.
     *
     * @tparam TSrc Source sample type
     * @tparam TDst Destination sample type
     * @param source Source samples span
     * @param destination Destination samples span
     * @param shift Number of bits to shift left
     */
    template<typename TSrc, typename TDst>
    static void pad(std::span<TSrc const> source, std::span<TDst> destination, std::uint8_t shift) noexcept
    {
      auto const count = std::min(source.size(), destination.size());

      for (std::size_t i = 0; i < count; ++i)
      {
        using UnsignedDestination = std::make_unsigned_t<TDst>;
        auto bits = static_cast<UnsignedDestination>(static_cast<TDst>(source[i]));
        bits <<= shift;
        destination[i] = std::bit_cast<TDst>(bits);
      }
    }

    /**
     * @brief Interleaves multiple non-interleaved channel buffers into a single destination buffer
     * while performing bit-depth padding.
     *
     * @tparam TSrc Source sample type
     * @tparam TDst Destination sample type
     * @param channels Array of spans, one for each channel
     * @param destination Destination interleaved buffer
     * @param shift Number of bits to shift left
     */
    template<typename TSrc, typename TDst>
    static void interleaveAndPad(std::span<std::span<TSrc const> const> channels,
                                 std::span<TDst> destination,
                                 std::uint8_t shift) noexcept
    {
      if (channels.empty())
      {
        return;
      }

      auto const channelCount = channels.size();
      auto const frameCount = std::min(channels[0].size(), destination.size() / channelCount);

      for (std::size_t i = 0; i < frameCount; ++i)
      {
        for (std::size_t ch = 0; ch < channelCount; ++ch)
        {
          using UnsignedDestination = std::make_unsigned_t<TDst>;
          auto bits = static_cast<UnsignedDestination>(static_cast<TDst>(channels[ch][i]));
          bits <<= shift;
          destination[(i * channelCount) + ch] = std::bit_cast<TDst>(bits);
        }
      }
    }

    /**
     * @brief Unpacks 24-bit PCM (3 bytes per sample) into 32-bit signed integers.
     *
     * @param source Source bytes (must be multiple of 3)
     * @param destination Destination samples (must have size source.size() / 3)
     * @param shift Optional additional shift for 24->32 alignment (usually 8)
     */
    static void unpackS24(std::span<std::byte const> source,
                          std::span<std::int32_t> destination,
                          std::uint8_t shift = 0) noexcept
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
  };
} // namespace ao::audio
