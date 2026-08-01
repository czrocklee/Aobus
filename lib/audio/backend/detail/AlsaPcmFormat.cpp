// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "AlsaPcmFormat.h"

#include <ao/audio/SampleEncoding.h>

extern "C"
{
#include <alsa/asoundlib.h>
}

#include <optional>

namespace ao::audio::backend::detail
{
  std::optional<::snd_pcm_format_t> alsaFormatFromSampleEncoding(SampleEncoding const encoding) noexcept
  {
    switch (encoding)
    {
      case SampleEncoding::Signed16Le: return SND_PCM_FORMAT_S16_LE;
      case SampleEncoding::Signed24PackedLe: return SND_PCM_FORMAT_S24_3LE;
      case SampleEncoding::Signed24In32Le: return SND_PCM_FORMAT_S24_LE;
      case SampleEncoding::Signed32Le: return SND_PCM_FORMAT_S32_LE;
      case SampleEncoding::Float32Le: return SND_PCM_FORMAT_FLOAT_LE;
      case SampleEncoding::Unknown: return std::nullopt;
    }

    return std::nullopt;
  }

  std::optional<SampleEncoding> sampleEncodingFromAlsaFormat(::snd_pcm_format_t const format) noexcept
  {
    switch (format)
    {
      case SND_PCM_FORMAT_S16_LE: return SampleEncoding::Signed16Le;
      case SND_PCM_FORMAT_S24_3LE: return SampleEncoding::Signed24PackedLe;
      case SND_PCM_FORMAT_S24_LE: return SampleEncoding::Signed24In32Le;
      case SND_PCM_FORMAT_S32_LE: return SampleEncoding::Signed32Le;
      case SND_PCM_FORMAT_FLOAT_LE: return SampleEncoding::Float32Le;
      default: return std::nullopt;
    }
  }
} // namespace ao::audio::backend::detail
