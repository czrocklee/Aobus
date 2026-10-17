// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <bit>
#include <cstdint>
#include <utility>

namespace ao::audio
{
  enum class SampleKind : std::uint8_t
  {
    Integer,
    FloatingPoint,
  };

  // The assigned WAVE speaker-position universe: the eighteen defined channel
  // bits in ascending order, excluding reserved positions and the SPEAKER_ALL
  // wildcard bit. Codec admission and default policy stay with each decoder.
  inline constexpr std::uint32_t kWaveSpeakerMask = 0x0003FFFF;

  // True when a non-zero mask assigns exactly one position per channel
  // within kWaveSpeakerMask. Zero is never complete, not even for zero
  // channels: its interpretation stays codec-local. The predicate judges an
  // already-normalized mask only; absence, defaults, and wildcards remain
  // codec-specific policy.
  constexpr bool isCompleteWaveSpeakerMask(std::uint32_t const mask, std::uint32_t const channels) noexcept
  {
    return mask != 0U && (mask & ~kWaveSpeakerMask) == 0U && std::cmp_equal(std::popcount(mask), channels);
  }

  /** @brief Logical audio signal properties, independent of PCM storage. */
  struct SignalFormat final
  {
    std::uint32_t sampleRate = 0;
    std::uint8_t channels = 0;
    std::uint8_t precisionBits = 0;
    SampleKind sampleKind = SampleKind::Integer;
    // Effective WAVE-style speaker identities in ascending assigned-bit order.
    // Zero assigns no speakers; channel count remains authoritative.
    std::uint32_t speakerMask = 0;

    bool operator==(SignalFormat const&) const = default;
  };
} // namespace ao::audio
