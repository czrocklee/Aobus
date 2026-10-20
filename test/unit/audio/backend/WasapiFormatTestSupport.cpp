// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "WasapiFormatTestSupport.h"

#include <ks.h>
#include <ksmedia.h>

namespace ao::audio::backend::test
{
  WAVEFORMATEXTENSIBLE extensibleFloatStereoWave()
  {
    auto wave = WAVEFORMATEXTENSIBLE{};
    wave.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    wave.Format.nChannels = 2;
    wave.Format.nSamplesPerSec = 48000;
    wave.Format.wBitsPerSample = 32;
    wave.Format.nBlockAlign = 8;
    wave.Format.nAvgBytesPerSec = 384000;
    wave.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    // WAVEFORMATEXTENSIBLE exposes valid precision through the Windows SDK union.
    wave.Samples.wValidBitsPerSample = 32; // NOLINT(cppcoreguidelines-pro-type-union-access)
    wave.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    wave.dwChannelMask = 3;
    return wave;
  }
} // namespace ao::audio::backend::test
