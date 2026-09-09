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
    CHECK(tail.start(100U, 24U) == DrainTailEvent::None);
    CHECK(tail.isActive());
    CHECK(tail.remainingFrames() == 76U);
    CHECK(tail.consume(64U) == DrainTailEvent::None);
    CHECK(tail.remainingFrames() == 12U);
    CHECK(tail.consume(64U) == DrainTailEvent::Completed);
    CHECK(tail.remainingFrames() == 0U);
    CHECK(tail.consume(64U) == DrainTailEvent::None);
  }

  TEST_CASE("AudioBackendDrainTail - a complete suffix can finish immediately", "[audio][unit][drain-tail]")
  {
    auto tail = AudioBackendDrainTail{};
    CHECK(tail.start(10U, 12U) == DrainTailEvent::Completed);
    CHECK(tail.isActive());
    CHECK(tail.remainingFrames() == 0U);
    CHECK(tail.consume(1U) == DrainTailEvent::None);
    tail.reset();
    CHECK_FALSE(tail.isActive());
    CHECK(tail.remainingFrames() == 0U);
    CHECK(tail.consume(1U) == DrainTailEvent::None);
  }

  TEST_CASE("AudioBackendDrainTail - only an active drain can produce a completion event", "[audio][unit][drain-tail]")
  {
    auto tail = AudioBackendDrainTail{};
    CHECK(tail.consume(100U) == DrainTailEvent::None);
    CHECK(tail.start(12U, 0U) == DrainTailEvent::None);
    CHECK(tail.consume(0U) == DrainTailEvent::None);
    CHECK(tail.remainingFrames() == 12U);
    tail.reset();
    CHECK(tail.consume(12U) == DrainTailEvent::None);
    CHECK(tail.start(0U, 0U) == DrainTailEvent::Completed);
    CHECK(tail.consume(12U) == DrainTailEvent::None);
  }
} // namespace ao::audio::backend::detail::test
