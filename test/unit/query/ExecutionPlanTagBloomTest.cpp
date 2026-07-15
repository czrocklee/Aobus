// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/query/ExecutionPlanTestSupport.h"
#include <ao/query/detail/Bytecode.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace ao::query::test
{
  TEST_CASE("ExecutionPlan - records exact required tag symbols", "[query][unit][execution-plan]")
  {
    SECTION("Single tag")
    {
      auto const plan = compileOk(QueryCompiler{}, parseOk("#rock"));
      CHECK(plan.dictionarySymbols == std::vector<std::string>{"rock"});
      CHECK(plan.requiredTagSymbols == std::vector<std::uint32_t>{0});
    }

    SECTION("AND requires both tags")
    {
      auto const plan = compileOk(QueryCompiler{}, parseOk("#rock and #jazz"));
      CHECK(plan.dictionarySymbols == std::vector<std::string>{"rock", "jazz"});
      CHECK(plan.requiredTagSymbols == std::vector<std::uint32_t>{0, 1});
    }

    SECTION("OR has no tag required by every branch")
    {
      auto const plan = compileOk(QueryCompiler{}, parseOk("#rock or #jazz"));
      CHECK(plan.requiredTagSymbols.empty());
    }

    SECTION("OR retains a tag shared by every branch")
    {
      auto const plan = compileOk(QueryCompiler{}, parseOk("(#rock and #jazz) or (#rock and #blues)"));
      REQUIRE_FALSE(plan.requiredTagSymbols.empty());
      CHECK(plan.dictionarySymbols[plan.requiredTagSymbols.front()] == "rock");
    }

    SECTION("NOT has no positive required tag")
    {
      auto const plan = compileOk(QueryCompiler{}, parseOk("not #rock"));
      CHECK(plan.requiredTagSymbols.empty());
    }
  }
} // namespace ao::query::test
