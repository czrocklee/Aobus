// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/winui/app/StartupWarnings.h>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>

namespace ao::winui::test
{
  TEST_CASE("StartupWarnings - the status line joins every warning in the order it arose")
  {
    auto warnings = StartupWarnings{};
    CHECK(warnings.statusLine().empty());

    CHECK(warnings.add("Settings were ignored.") == "Settings were ignored.");
    CHECK(warnings.add("The theme was rejected.") == "Settings were ignored. The theme was rejected.");
    CHECK(warnings.statusLine() == "Settings were ignored. The theme was rejected.");
  }

  TEST_CASE("StartupWarnings - only a startup scan that did not fail gives the line back once")
  {
    auto warnings = StartupWarnings{};

    SECTION("the settled startup scan restores the warnings")
    {
      warnings.add("Settings were ignored.");
      warnings.add("The theme was rejected.");

      CHECK(warnings.settle(true, false) ==
            std::optional<std::string>{"Settings were ignored. The theme was rejected."});
      CHECK(warnings.settle(true, false) == std::nullopt);
    }

    SECTION("a failed startup scan keeps the line for its error")
    {
      warnings.add("Settings were ignored.");

      CHECK(warnings.settle(true, true) == std::nullopt);
      CHECK(warnings.settle(true, false) == std::nullopt);
    }

    SECTION("a later scan in a session that did not scan at open restores nothing")
    {
      warnings.add("Settings were ignored.");

      CHECK(warnings.settle(false, false) == std::nullopt);
    }

    SECTION("a startup scan without warnings keeps its own status")
    {
      CHECK(warnings.settle(true, false) == std::nullopt);
    }

    CHECK(warnings.statusLine().empty());
  }
} // namespace ao::winui::test
