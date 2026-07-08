// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "test/unit/query/ExecutionPlanTestSupport.h"
#include <ao/query/detail/Bytecode.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <tuple>

namespace ao::query::test
{
  TEST_CASE("ExecutionPlan - compiles in lists", "[query][unit][execution-plan][membership]")
  {
    auto compiler = QueryCompiler{};

    SECTION("CompilesConstantListAsMembershipSet")
    {
      auto const plan = compileOk(compiler, parseOk("$year in [1990, 1991, 1992]"));

      REQUIRE(plan.inSets.size() == 1);
      CHECK(plan.inSets[0].numericValues.contains(1991));
      CHECK(plan.inSets[0].numericValues.contains(2000) == false);
      CHECK(std::ranges::count(plan.instructions, OpCode::InSet, &Instruction::op) == 1);
      CHECK(std::ranges::count(plan.instructions, OpCode::Eq, &Instruction::op) == 0);
      CHECK(std::ranges::count(plan.instructions, OpCode::Or, &Instruction::op) == 0);
    }

    SECTION("CompilesSingleItemListAsMembershipSet")
    {
      auto const plan = compileOk(compiler, parseOk("$year in [1990]"));

      REQUIRE(plan.inSets.size() == 1);
      CHECK(plan.inSets[0].numericValues.contains(1990));
      CHECK(std::ranges::count(plan.instructions, OpCode::InSet, &Instruction::op) == 1);
      CHECK(std::ranges::count(plan.instructions, OpCode::Eq, &Instruction::op) == 0);
      CHECK(std::ranges::count(plan.instructions, OpCode::Or, &Instruction::op) == 0);
    }

    SECTION("RejectsStandaloneList")
    {
      std::ignore = compileError(compiler, parseOk("[1990, 1991]"));
    }

    SECTION("RejectsNonListRightOperand")
    {
      std::ignore = compileError(compiler, parseOk("$artist in Bach"));
    }
  }

  TEST_CASE("ExecutionPlan - compiles in ranges", "[query][unit][execution-plan][membership]")
  {
    auto compiler = QueryCompiler{};

    SECTION("CompilesRangeAsClosedBounds")
    {
      auto const plan = compileOk(compiler, parseOk("$year in 1990..1999"));

      CHECK(std::ranges::count(plan.instructions, OpCode::Ge, &Instruction::op) == 1);
      CHECK(std::ranges::count(plan.instructions, OpCode::Le, &Instruction::op) == 1);
      CHECK(std::ranges::count(plan.instructions, OpCode::And, &Instruction::op) == 1);
    }

    SECTION("ScalesDurationRangeBounds")
    {
      auto const plan = compileOk(compiler, parseOk("@duration in 2m30s..5m"));

      REQUIRE(plan.instructions.size() >= 5);
      CHECK(plan.instructions[1].op == OpCode::LoadConstant);
      CHECK(plan.instructions[1].constValue == 150000);
      CHECK(plan.instructions[4].op == OpCode::LoadConstant);
      CHECK(plan.instructions[4].constValue == 300000);
    }

    SECTION("RejectsStandaloneRange")
    {
      std::ignore = compileError(compiler, parseOk("1990..1999"));
    }

    SECTION("CompilesDictionaryRangeAsStringBounds")
    {
      // Dictionary fields hold interned IDs, so range bounds are kept as string
      // constants (not resolved to IDs) for lexicographic comparison at eval time.
      auto const plan = compileOk(compiler, parseOk("$artist in Bach..Mozart"));

      REQUIRE(plan.stringConstants.size() == 2);
      CHECK(plan.stringConstants[0] == "Bach");
      CHECK(plan.stringConstants[1] == "Mozart");
      CHECK(std::ranges::count(plan.instructions, OpCode::Ge, &Instruction::op) == 1);
      CHECK(std::ranges::count(plan.instructions, OpCode::Le, &Instruction::op) == 1);
    }

    SECTION("RejectsNonStringDictionaryRangeBounds")
    {
      // An ordered comparison over a dictionary field only makes sense against text.
      std::ignore = compileError(compiler, parseOk("$artist in 1..5"));
    }

    SECTION("AllowsRangeOnStringField")
    {
      // Plain string fields compare lexicographically, so a range is meaningful.
      std::ignore = compileOk(compiler, parseOk("$title in apple..zoo"));
    }
  }

  TEST_CASE("ExecutionPlan - enforces ordered comparison field restrictions",
            "[query][unit][execution-plan][membership]")
  {
    auto compiler = QueryCompiler{};

    SECTION("AllowsStringRelationalOnDictionaryField")
    {
      // Ordered comparisons over dictionary fields compare resolved text; the
      // operand is kept as a string constant rather than resolved to an ID.
      auto const plan = compileOk(compiler, parseOk("$artist > Bach"));

      REQUIRE(plan.stringConstants.size() == 1);
      CHECK(plan.stringConstants[0] == "Bach");
      CHECK(std::ranges::count(plan.instructions, OpCode::Gt, &Instruction::op) == 1);
    }

    SECTION("RejectsNonStringRelationalOnDictionaryField")
    {
      std::ignore = compileError(compiler, parseOk("$artist > 5"));
      std::ignore = compileError(compiler, parseOk("$genre <= 3m"));
    }

    SECTION("AllowsEqualityOnDictionaryField")
    {
      std::ignore = compileOk(compiler, parseOk("$artist = Bach"));
      std::ignore = compileOk(compiler, parseOk("$genre in [Classical, Jazz]"));
    }

    SECTION("AllowsRelationalOnNumericAndStringFields")
    {
      std::ignore = compileOk(compiler, parseOk("$year < 1990"));
      std::ignore = compileOk(compiler, parseOk("$title < zoo"));
      std::ignore = compileOk(compiler, parseOk("$coverArt > 0"));
    }
  }
} // namespace ao::query::test
