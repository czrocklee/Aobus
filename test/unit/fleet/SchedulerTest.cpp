// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "fleet/Engine.h"
#include "fleet/Model.h"
#include "fleet/Serialization.h"
#include "test/fleet/TestUtils.h"

#include <catch2/catch_test_macros.hpp>

#include <vector>

namespace ao::fleet::test
{
  TEST_CASE("Fleet scheduler - intent sets reject ambiguous dependency graphs", "[fleet][unit][scheduler]")
  {
    auto temp = TempDir{};

    SECTION("duplicate phase IDs")
    {
      auto paths = std::vector{writeFile(temp, "one.yaml", intentYaml()), writeFile(temp, "two.yaml", intentYaml())};
      auto result = loadIntents(paths);
      REQUIRE_FALSE(result);
      CHECK(result.error().message.find("duplicate intent id") != std::string::npos);
    }

    SECTION("dependencies on missing phases")
    {
      auto paths = std::vector{writeFile(temp, "one.yaml", intentYaml("phase-a", "missing"))};
      auto result = loadIntents(paths);
      REQUIRE_FALSE(result);
      CHECK(result.error().message.find("dangling dependency") != std::string::npos);
    }
  }

  TEST_CASE("Fleet scheduler - dependency graph validation and cycle rejection", "[fleet][unit][scheduler]")
  {
    auto first = PhaseIntent{.id = "a",
                             .taskKind = "test",
                             .invariant = "test",
                             .scope = {},
                             .dependsOn = {},
                             .overrides = {},
                             .body = "test"};
    auto second = PhaseIntent{.id = "b",
                              .taskKind = "test",
                              .invariant = "test",
                              .scope = {},
                              .dependsOn = {"a"},
                              .overrides = {},
                              .body = "test"};
    auto third = PhaseIntent{.id = "c",
                             .taskKind = "test",
                             .invariant = "test",
                             .scope = {},
                             .dependsOn = {"b"},
                             .overrides = {},
                             .body = "test"};

    SECTION("accepts a DAG")
    {
      CHECK(Scheduler::validate({third, first, second}));
    }

    SECTION("rejects a cycle")
    {
      first.dependsOn = {"c"};
      auto result = Scheduler::validate({first, second, third});
      REQUIRE_FALSE(result);
      CHECK(result.error().message.find("cycle") != std::string::npos);
    }
  }
} // namespace ao::fleet::test
