// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "WasapiFormatObservation.h"
#include <ao/Error.h>
#include <ao/audio/ChannelDeliveryEvidence.h>
#include <ao/audio/PcmFormat.h>

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

#include <optional>

namespace ao::audio::backend::detail
{
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
