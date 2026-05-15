// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/audio/backend/detail/PipeWireShared.h>
#include <catch2/catch_test_macros.hpp>

#include <optional>

namespace ao::audio::backend::detail::test
{
  TEST_CASE("PipeWireShared - Property Parsing", "[audio][unit][pipewire]")
  {
    SECTION("parseUintProperty strictness")
    {
      REQUIRE(parseUintProperty(nullptr) == std::nullopt);
      REQUIRE(parseUintProperty("") == std::nullopt);
      REQUIRE(parseUintProperty("abc") == std::nullopt);
      REQUIRE(parseUintProperty("12abc") == std::nullopt);
      REQUIRE(parseUintProperty("abc12") == std::nullopt);
      REQUIRE(parseUintProperty(" 12") == std::nullopt); // strtoul allows leading space, but we want strictness?
      // Actually strtoul skips leading space. Let's see what we want.
      // The plan says "accept only full decimal strings".

      REQUIRE(parseUintProperty("42") == 42);
      REQUIRE(parseUintProperty("0") == 0);
      REQUIRE(parseUintProperty("4294967295") == 4294967295U);
    }
  }
} // namespace ao::audio::backend::detail::test
