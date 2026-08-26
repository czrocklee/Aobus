// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "CoreAudioLatency.h"

#include <ao/Error.h>

#include <cmath>
#include <cstdint>
#include <limits>

namespace ao::audio::backend::detail
{
  Result<std::uint64_t> coreAudioPresentationTailFrames(CoreAudioLatencyComponents const& components)
  {
    if (components.deviceSampleRate <= 0.0 || !std::isfinite(components.deviceSampleRate) ||
        components.clientSampleRate == 0U || components.audioUnitLatencySeconds < 0.0 ||
        !std::isfinite(components.audioUnitLatencySeconds))
    {
      return makeError(
        Error::Code::InvalidInput, "Core Audio latency requires finite non-negative values and sample rates");
    }

    auto const deviceFrames = static_cast<long double>(components.ioBufferFrames) +
                              static_cast<long double>(components.safetyOffsetFrames) +
                              static_cast<long double>(components.deviceLatencyFrames) +
                              static_cast<long double>(components.streamLatencyFrames);
    auto const seconds = (deviceFrames / static_cast<long double>(components.deviceSampleRate)) +
                         static_cast<long double>(components.audioUnitLatencySeconds);
    auto const clientFrames = std::ceil(seconds * static_cast<long double>(components.clientSampleRate));

    if (clientFrames >= static_cast<long double>(std::numeric_limits<std::uint64_t>::max()))
    {
      return std::numeric_limits<std::uint64_t>::max();
    }

    return static_cast<std::uint64_t>(clientFrames);
  }
} // namespace ao::audio::backend::detail
