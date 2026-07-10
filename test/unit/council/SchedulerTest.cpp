// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "council/CouncilSchema.h"
#include "council/Engine.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <utility>

namespace ao::council::test
{
  namespace
  {
    PhaseIntent phase(std::string id)
    {
      return PhaseIntent{.id = std::move(id),
                         .taskKind = "council-review",
                         .invariant = "Preserve behavior.",
                         .body = "Review the change."};
    }
  } // namespace

  TEST_CASE("Scheduler - rejects duplicate phase ids", "[council][unit][scheduler]")
  {
    auto first = phase("phase-a");
    auto second = phase("phase-a");

    auto result = Scheduler::validate({first, second});

    REQUIRE_FALSE(result);
    CHECK(result.error().message.contains("duplicate phase id"));
  }

  TEST_CASE("Scheduler - rejects unknown dependencies", "[council][unit][scheduler]")
  {
    auto item = phase("phase-a");
    item.dependsOn = {"missing"};

    auto result = Scheduler::validate({item});

    REQUIRE_FALSE(result);
    CHECK(result.error().message.contains("unknown phase"));
  }

  TEST_CASE("Scheduler - rejects dependency cycles", "[council][unit][scheduler]")
  {
    auto first = phase("phase-a");
    auto second = phase("phase-b");
    first.dependsOn = {"phase-b"};
    second.dependsOn = {"phase-a"};

    auto result = Scheduler::validate({first, second});

    REQUIRE_FALSE(result);
    CHECK(result.error().message.contains("dependency cycle"));
  }
} // namespace ao::council::test
