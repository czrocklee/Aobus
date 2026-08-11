// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/audio/SampleEncoding.h>

#include <cstddef>
#include <span>

namespace ao::audio::backend::detail
{
  /**
   * @brief Applies software gain to a buffer of ALSA PCM samples.
   *
   * @param pcm The buffer of interleaved PCM samples.
   * @param encoding The byte layout and integer scale of each sample.
   * @param gain The gain to apply [0.0, 1.0].
   */
  void applyAlsaSoftwareGain(std::span<std::byte> pcm, SampleEncoding encoding, float gain) noexcept;
} // namespace ao::audio::backend::detail
