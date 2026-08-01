// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/audio/SampleEncoding.h>

extern "C"
{
#include <alsa/asoundlib.h>
}

#include <array>
#include <optional>

namespace ao::audio::backend::detail
{
  /** @brief Every encoding this backend can map, ordered narrowest container first. */
  inline constexpr auto kAlsaCandidateEncodings = std::array{SampleEncoding::Signed16Le,
                                                             SampleEncoding::Signed24PackedLe,
                                                             SampleEncoding::Signed24In32Le,
                                                             SampleEncoding::Signed32Le,
                                                             SampleEncoding::Float32Le};

  std::optional<::snd_pcm_format_t> alsaFormatFromSampleEncoding(SampleEncoding encoding) noexcept;

  std::optional<SampleEncoding> sampleEncodingFromAlsaFormat(::snd_pcm_format_t format) noexcept;
} // namespace ao::audio::backend::detail
