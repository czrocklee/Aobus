// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/Error.h>

#include <cstdint>

namespace ao::audio::backend::detail
{
  struct CoreAudioLatencyComponents final
  {
    std::uint64_t ioBufferFrames = 0U;
    std::uint64_t safetyOffsetFrames = 0U;
    std::uint64_t deviceLatencyFrames = 0U;
    std::uint64_t streamLatencyFrames = 0U;
    double audioUnitLatencySeconds = 0.0;
    double deviceSampleRate = 0.0;
    std::uint32_t clientSampleRate = 0U;
  };

  Result<std::uint64_t> coreAudioPresentationTailFrames(CoreAudioLatencyComponents const& components);
} // namespace ao::audio::backend::detail
