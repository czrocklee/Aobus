// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "lib/audio/backend/detail/AudioBackendDrainTail.h"

#include <catch2/catch_test_macros.hpp>

namespace ao::audio::backend::detail::test
{
  TEST_CASE("AudioBackendDrainTail - final callback silence contributes to presentation drain",
            "[audio][unit][drain-tail]")
  {
    auto tail = AudioBackendDrainTail{};
    CHECK_FALSE(tail.start(100U, 24U));
    CHECK(tail.active());
    CHECK(tail.remainingFrames() == 76U);
    CHECK_FALSE(tail.consume(64U));
    CHECK(tail.remainingFrames() == 12U);
    CHECK(tail.consume(64U));
    CHECK(tail.remainingFrames() == 0U);
    CHECK_FALSE(tail.consume(64U));
  }

  TEST_CASE("AudioBackendDrainTail - a complete suffix can finish immediately", "[audio][unit][drain-tail]")
  {
    auto tail = AudioBackendDrainTail{};
    CHECK(tail.start(10U, 12U));
    tail.reset();
    CHECK_FALSE(tail.active());
    CHECK(tail.remainingFrames() == 0U);
  }
} // namespace ao::audio::backend::detail::test
