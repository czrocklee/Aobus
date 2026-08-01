// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "lib/audio/backend/detail/AlsaPcmFormat.h"

#include <ao/audio/SampleEncoding.h>

#include <catch2/catch_test_macros.hpp>

extern "C"
{
#include <alsa/asoundlib.h>
}

namespace ao::audio::backend::detail::test
{
  TEST_CASE("AlsaPcmFormat - maps sample encodings to exact ALSA formats", "[audio][unit][alsa]")
  {
    CHECK(alsaFormatFromSampleEncoding(SampleEncoding::Signed16Le) == SND_PCM_FORMAT_S16_LE);
    CHECK(alsaFormatFromSampleEncoding(SampleEncoding::Signed24PackedLe) == SND_PCM_FORMAT_S24_3LE);
    CHECK(alsaFormatFromSampleEncoding(SampleEncoding::Signed24In32Le) == SND_PCM_FORMAT_S24_LE);
    CHECK(alsaFormatFromSampleEncoding(SampleEncoding::Signed32Le) == SND_PCM_FORMAT_S32_LE);
    CHECK(alsaFormatFromSampleEncoding(SampleEncoding::Float32Le) == SND_PCM_FORMAT_FLOAT_LE);
    CHECK_FALSE(alsaFormatFromSampleEncoding(SampleEncoding::Unknown));
  }

  TEST_CASE("AlsaPcmFormat - maps ALSA formats to exact sample encodings", "[audio][unit][alsa]")
  {
    CHECK(sampleEncodingFromAlsaFormat(SND_PCM_FORMAT_S16_LE) == SampleEncoding::Signed16Le);
    CHECK(sampleEncodingFromAlsaFormat(SND_PCM_FORMAT_S24_3LE) == SampleEncoding::Signed24PackedLe);
    CHECK(sampleEncodingFromAlsaFormat(SND_PCM_FORMAT_S24_LE) == SampleEncoding::Signed24In32Le);
    CHECK(sampleEncodingFromAlsaFormat(SND_PCM_FORMAT_S32_LE) == SampleEncoding::Signed32Le);
    CHECK(sampleEncodingFromAlsaFormat(SND_PCM_FORMAT_FLOAT_LE) == SampleEncoding::Float32Le);
    CHECK_FALSE(sampleEncodingFromAlsaFormat(SND_PCM_FORMAT_UNKNOWN));
  }
} // namespace ao::audio::backend::detail::test
