// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "lib/audio/backend/detail/CoreAudioLatency.h"

#include <ao/Error.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <limits>

namespace ao::audio::backend::detail::test
{
  TEST_CASE("CoreAudioLatency - converts the complete device and AudioUnit tail to client frames",
            "[audio][unit][coreaudio]")
  {
    auto const result = coreAudioPresentationTailFrames({.ioBufferFrames = 480,
                                                          .safetyOffsetFrames = 48,
                                                          .deviceLatencyFrames = 24,
                                                          .streamLatencyFrames = 8,
                                                          .audioUnitLatencySeconds = 0.001,
                                                          .deviceSampleRate = 48000.0,
                                                          .clientSampleRate = 44100});
    REQUIRE(result);
    CHECK(*result == 559U);
  }

  TEST_CASE("CoreAudioLatency - rounds any partial client frame upward", "[audio][unit][coreaudio]")
  {
    auto const result = coreAudioPresentationTailFrames(
      {.ioBufferFrames = 1, .deviceSampleRate = 48000.0, .clientSampleRate = 44100});
    REQUIRE(result);
    CHECK(*result == 1U);
  }

  TEST_CASE("CoreAudioLatency - rejects invalid timing evidence and saturates overflow",
            "[audio][unit][coreaudio]")
  {
    auto const invalidRes = coreAudioPresentationTailFrames({.deviceSampleRate = 0.0, .clientSampleRate = 48000});
    REQUIRE_FALSE(invalidRes);
    CHECK(invalidRes.error().code == Error::Code::InvalidInput);

    auto const maximum = std::numeric_limits<std::uint64_t>::max();
    auto const saturatedRes = coreAudioPresentationTailFrames({.ioBufferFrames = maximum,
                                                                .safetyOffsetFrames = maximum,
                                                                .deviceLatencyFrames = maximum,
                                                                .streamLatencyFrames = maximum,
                                                                .deviceSampleRate = 1.0,
                                                                .clientSampleRate =
                                                                  std::numeric_limits<std::uint32_t>::max()});
    REQUIRE(saturatedRes);
    CHECK(*saturatedRes == maximum);
  }
} // namespace ao::audio::backend::detail::test
