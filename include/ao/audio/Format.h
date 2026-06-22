// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <cstddef>
#include <cstdint>

namespace ao::audio
{
  /**
   * @brief Describes the physical format of a PCM audio stream.
   */
  struct Format final
  {
    std::uint32_t sampleRate = 0;
    std::uint8_t channels = 0;
    std::uint8_t bitDepth = 0;
    std::uint8_t validBits = 0;
    bool isFloat = false;
    bool isInterleaved = true;

    bool operator==(Format const&) const = default;
  };

  /**
   * @brief Storage size in bytes of one sample of the given format.
   *
   * Maps the logical bit depth onto the container width the decoders emit:
   * <=16 bits pack into 16-bit samples, 24-bit stays packed in 3 bytes, and
   * everything wider (including 32-bit float) uses 4 bytes.
   */
  constexpr std::uint32_t bytesPerSample(Format const& format) noexcept
  {
    if (format.bitDepth <= 16U)
    {
      return 2U;
    }

    if (format.bitDepth == 24U)
    {
      return 3U;
    }

    return 4U;
  }

  /**
   * @brief Size in bytes of one interleaved frame (all channels). Returns 0 for
   * an unconfigured format (no channels or bit depth).
   */
  constexpr std::size_t frameBytes(Format const& format) noexcept
  {
    if (format.channels == 0U || format.bitDepth == 0U)
    {
      return 0U;
    }

    return static_cast<std::size_t>(format.channels) * bytesPerSample(format);
  }

  /**
   * @brief PCM byte throughput for one second of the given format. Returns 0 for
   * an unconfigured format (no sample rate, channels, or bit depth).
   */
  constexpr std::uint64_t bytesPerSecond(Format const& format) noexcept
  {
    if (format.sampleRate == 0U)
    {
      return 0U;
    }

    return static_cast<std::uint64_t>(format.sampleRate) * frameBytes(format);
  }
} // namespace ao::audio