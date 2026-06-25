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
      CHECK(parseUintProperty(nullptr) == std::nullopt);
      CHECK(parseUintProperty("") == std::nullopt);
      CHECK(parseUintProperty("abc") == std::nullopt);
      CHECK(parseUintProperty("12abc") == std::nullopt);
      CHECK(parseUintProperty("abc12") == std::nullopt);
      CHECK(parseUintProperty(" 12") == std::nullopt);
      CHECK(parseUintProperty("4294967296") == std::nullopt);

      CHECK(parseUintProperty("42") == 42);
      CHECK(parseUintProperty("0") == 0);
      CHECK(parseUintProperty("4294967295") == 4294967295U);
    }
  }
} // namespace ao::audio::backend::detail::test
