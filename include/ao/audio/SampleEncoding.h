// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <cstdint>
#include <string_view>

namespace ao::audio
{
  /**
   * @brief Byte-level identity of one interleaved little-endian PCM sample.
   *
   * The encoding owns container width and integer alignment. In particular,
   * Signed24In32Le stores a sign-extended 24-bit value in the low 24 bits of a
   * four-byte container; Signed32Le uses the full 32-bit integer scale.
   */
  enum class SampleEncoding : std::uint8_t
  {
    Unknown,
    Signed16Le,
    Signed24PackedLe,
    Signed24In32Le,
    Signed32Le,
    Float32Le,
  };

  constexpr std::uint8_t encodingNominalBits(SampleEncoding const encoding) noexcept
  {
    switch (encoding)
    {
      case SampleEncoding::Signed16Le: return 16U;
      case SampleEncoding::Signed24PackedLe:
      case SampleEncoding::Signed24In32Le: return 24U;
      case SampleEncoding::Signed32Le:
      case SampleEncoding::Float32Le: return 32U;
      case SampleEncoding::Unknown: return 0U;
    }

    return 0U;
  }

  constexpr std::uint8_t encodingContainerBits(SampleEncoding const encoding) noexcept
  {
    switch (encoding)
    {
      case SampleEncoding::Signed16Le: return 16U;
      case SampleEncoding::Signed24PackedLe: return 24U;
      case SampleEncoding::Signed24In32Le:
      case SampleEncoding::Signed32Le:
      case SampleEncoding::Float32Le: return 32U;
      case SampleEncoding::Unknown: return 0U;
    }

    return 0U;
  }

  constexpr std::uint32_t bytesPerSample(SampleEncoding const encoding) noexcept
  {
    return static_cast<std::uint32_t>(encodingContainerBits(encoding)) / 8U;
  }

  constexpr bool isFloatEncoding(SampleEncoding const encoding) noexcept
  {
    return encoding == SampleEncoding::Float32Le;
  }

  /** @brief Stable diagnostic name; not a persisted or user-facing string. */
  constexpr std::string_view sampleEncodingName(SampleEncoding const encoding) noexcept
  {
    switch (encoding)
    {
      case SampleEncoding::Signed16Le: return "S16_LE";
      case SampleEncoding::Signed24PackedLe: return "S24_3LE";
      case SampleEncoding::Signed24In32Le: return "S24_LE";
      case SampleEncoding::Signed32Le: return "S32_LE";
      case SampleEncoding::Float32Le: return "FLOAT_LE";
      case SampleEncoding::Unknown: return "unknown";
    }

    return "unknown";
  }
} // namespace ao::audio
