// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/audio/ChannelDeliveryEvidence.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/SignalFormat.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
// mmreg.h assumes the base Win32 types and pointer macros from windows.h.
// clang-format off
#include <windows.h>
#include <mmreg.h>
// clang-format on

#include <cstdint>
#include <optional>

namespace ao::audio::backend::detail
{
  struct WaveFormatObservation final
  {
    std::uint32_t sampleRate = 0;
    std::uint16_t channels = 0;
    // Effective observed roles, possibly partial; excess high positions are
    // ignored, and reserved or wildcard positions erase the whole role set.
    std::uint32_t speakerMask = 0;
    // A canonical format mask is present only for a complete observed set.
    std::optional<PcmFormat> optFormat{};
  };

  bool acceptsWaveSpeakerSet(SignalFormat const& sourceFormat) noexcept;
  std::optional<WAVEFORMATEXTENSIBLE> waveFormatFromPcmFormat(PcmFormat const& format) noexcept;
  ChannelDeliveryEvidence waveChannelDeliveryFromObservation(PcmFormat const& applicationFormat,
                                                             WAVEFORMATEXTENSIBLE const& acceptedClient,
                                                             WaveFormatObservation const& mix) noexcept;

  // Failure means a storage-integrity fault in a successful descriptor (a
  // short extensible object). Absent, contradictory or reserved optional
  // evidence is returned as an observation without a concrete format claim or
  // usable roles; it never vetoes a separately constructed client descriptor.
  Result<WaveFormatObservation> observeWaveFormat(WAVEFORMATEX const& wave);
} // namespace ao::audio::backend::detail
