// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace ao::audio::backend::detail
{
  /**
   * @brief Applies software gain to a buffer of ALSA PCM samples.
   *
   * @param pcm The buffer of interleaved PCM samples.
   * @param bitDepth The bit depth of the samples (16, 24, or 32).
   * @param validBits The number of valid bits in the container (e.g. 24 bits in a 32-bit container).
   * @param is3Byte24Bit True if the format is SND_PCM_FORMAT_S24_3LE (packed 24-bit).
   * @param gain The gain to apply [0.0, 1.0].
   */
  void applyAlsaSoftwareGain(std::span<std::byte> pcm,
                             std::uint8_t bitDepth,
                             std::uint8_t validBits,
                             bool is3Byte24Bit,
                             float gain) noexcept;
} // namespace ao::audio::backend::detail
