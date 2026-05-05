// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/utility/ByteView.h>

#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>

namespace ao::audio::pcm
{
  /**
   * @brief High-performance PCM format conversion utilities.
   */
  class Converter final
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
        destination[i] = static_cast<TDst>(source[i]) << shift;
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
      if (channels.empty()) return;

      auto const channelCount = channels.size();
      auto const frameCount = std::min(channels[0].size(), destination.size() / channelCount);

      for (std::size_t i = 0; i < frameCount; ++i)
      {
        for (std::size_t ch = 0; ch < channelCount; ++ch)
        {
          destination[i * channelCount + ch] = static_cast<TDst>(channels[ch][i]) << shift;
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

      for (std::size_t i = 0; i < count; ++i)
      {
        auto const offset = i * 3;

        // Manual unpack for little-endian S24
        std::int32_t val = static_cast<std::uint8_t>(source[offset]) |
                           (static_cast<std::uint8_t>(source[offset + 1]) << 8) |
                           (static_cast<std::uint8_t>(source[offset + 2]) << 16);

        // Sign extension from 24 to 32 bits
        if (val & 0x800000)
        {
          val |= 0xFF000000;
        }

        destination[i] = val << shift;
      }
    }
  };
} // namespace ao::audio::pcm
