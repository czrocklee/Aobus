// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/uimodel/input/KeyRepeatGuard.h>

#include <catch2/catch_test_macros.hpp>

namespace ao::uimodel::test
{
  TEST_CASE("KeyRepeatGuard - accepts one press until the physical key is released",
            "[uimodel][unit][input][key-repeat]")
  {
    auto guard = KeyRepeatGuard{};

    CHECK(guard.acceptPress(38));
    CHECK_FALSE(guard.acceptPress(38));
    CHECK(guard.acceptPress(40));

    guard.release(38);
    CHECK(guard.acceptPress(38));
    CHECK_FALSE(guard.acceptPress(40));
  }

  TEST_CASE("KeyRepeatGuard - reset starts a new key cycle", "[uimodel][unit][input][key-repeat]")
  {
    auto guard = KeyRepeatGuard{};

    REQUIRE(guard.acceptPress(38));
    guard.reset();
    CHECK(guard.acceptPress(38));
  }
} // namespace ao::uimodel::test
