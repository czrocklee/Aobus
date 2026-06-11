// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "lib/audio/detail/TimeConversion.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <limits>

namespace ao::audio::detail::test
{
  TEST_CASE("TimeConversion - scales timestamps without overflow", "[audio][unit][detail]")
  {
    CHECK(saturatingScale(44100, 1000, 44100) == 1000);
    CHECK(saturatingScale(22050, 48000, 44100) == 24000);
    CHECK(saturatingScale(1, 1, 0) == 0);
    CHECK(saturatingScale(std::numeric_limits<std::uint64_t>::max(), std::numeric_limits<std::uint32_t>::max(), 1) ==
          std::numeric_limits<std::uint64_t>::max());
  }

  TEST_CASE("TimeConversion - saturates public millisecond duration", "[audio][unit][detail]")
  {
    CHECK(durationMilliseconds(96000, 48000) == 2000);
    CHECK(durationMilliseconds(1, 0) == 0);
    CHECK(durationMilliseconds(std::numeric_limits<std::uint64_t>::max(), 1) ==
          std::numeric_limits<std::uint32_t>::max());
  }
} // namespace ao::audio::detail::test
