// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/audio/PcmFormat.h>

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
} // namespace ao::audio::backend::detail
