// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "test/unit/query/ExecutionPlanTestSupport.h"
#include <ao/query/Field.h>
#include <ao/query/detail/Bytecode.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <tuple>
#include <vector>

namespace ao::query::test
{
  namespace
  {
    InstructionIterator findOnlyInstruction(ExecutionPlan const& plan, OpCode op)
    {
      auto const instruction = std::ranges::find(plan.instructions, op, &Instruction::op);
      REQUIRE(instruction != plan.instructions.end());
      CHECK(std::ranges::count(plan.instructions, op, &Instruction::op) == 1);
      return instruction;
    }

    void checkYearMembership(ExecutionPlan const& plan)
    {
      auto const load = findOnlyInstruction(plan, OpCode::LoadField);
      auto const membership = findOnlyInstruction(plan, OpCode::InSet);
      CHECK(load < membership);
      CHECK(load->field == static_cast<std::uint8_t>(Field::Year));
      CHECK(membership->field == static_cast<std::uint8_t>(Field::Year));
      CHECK(load->operand == 0);
      CHECK(membership->operand == load->operand);
      CHECK(membership->constValue == 0);
      CHECK(membership->dictionarySymbol == kNoDictionarySymbol);
    }

    InstructionIterator checkBound(ExecutionPlan const& plan, OpCode op, Field field, std::int64_t value)
    {
      auto const comparison = findOnlyInstruction(plan, op);
      REQUIRE(comparison - plan.instructions.begin() >= 2);
      auto const& load = *(comparison - 2);
      auto const& constant = *(comparison - 1);
      REQUIRE(load.op == OpCode::LoadField);
      REQUIRE(constant.op == OpCode::LoadConstant);
      CHECK(load.field == static_cast<std::uint8_t>(field));
      CHECK(comparison->field == static_cast<std::uint8_t>(field));
      CHECK(constant.constValue == value);
      CHECK(constant.operand == load.operand + 1);
      CHECK(comparison->operand == constant.operand);
      CHECK(comparison->dictionarySymbol == kNoDictionarySymbol);
      return comparison;
    }

    void checkClosedRange(ExecutionPlan const& plan, Field field, std::int64_t lower, std::int64_t upper)
    {
      auto const lowerBound = checkBound(plan, OpCode::Ge, field, lower);
      auto const upperBound = checkBound(plan, OpCode::Le, field, upper);
      auto const conjunction = findOnlyInstruction(plan, OpCode::And);
      CHECK(lowerBound < upperBound);
      CHECK(upperBound < conjunction);
      CHECK(lowerBound->operand == 1);
      CHECK(upperBound->operand == lowerBound->operand + 1);
      CHECK(conjunction->operand == upperBound->operand - 1);
    }
  } // namespace

  TEST_CASE("ExecutionPlan - compiles in lists", "[query][unit][execution-plan][membership]")
  {
    SECTION("CompilesConstantListAsMembershipSet")
    {
      auto const plan = compileOk(parseOk("$year in [1990, 1991, 1992]"));

      REQUIRE(plan.inSets.size() == 1);
      CHECK(plan.inSets[0].valueKind == InSetValueKind::Numeric);
      CHECK(plan.inSets[0].numericValues.size() == 3);
      CHECK(plan.inSets[0].numericValues.contains(1990));
      CHECK(plan.inSets[0].numericValues.contains(1991));
      CHECK(plan.inSets[0].numericValues.contains(1992));
      CHECK(plan.inSets[0].numericValues.contains(2000) == false);
      CHECK(plan.inSets[0].strings.empty());
      CHECK(plan.inSets[0].dictionarySymbols.empty());
      checkYearMembership(plan);
      CHECK(std::ranges::count(plan.instructions, OpCode::Eq, &Instruction::op) == 0);
      CHECK(std::ranges::count(plan.instructions, OpCode::Or, &Instruction::op) == 0);
    }

    SECTION("CompilesSingleItemListAsMembershipSet")
    {
      auto const plan = compileOk(parseOk("$year in [1990]"));

      REQUIRE(plan.inSets.size() == 1);
      CHECK(plan.inSets[0].valueKind == InSetValueKind::Numeric);
      CHECK(plan.inSets[0].numericValues.size() == 1);
      CHECK(plan.inSets[0].numericValues.contains(1990));
      CHECK(plan.inSets[0].strings.empty());
      CHECK(plan.inSets[0].dictionarySymbols.empty());
      checkYearMembership(plan);
      CHECK(std::ranges::count(plan.instructions, OpCode::Eq, &Instruction::op) == 0);
      CHECK(std::ranges::count(plan.instructions, OpCode::Or, &Instruction::op) == 0);
    }

    SECTION("RejectsStandaloneList")
    {
      std::ignore = compileError(parseOk("[1990, 1991]"));
    }

    SECTION("RejectsNonListRightOperand")
    {
      std::ignore = compileError(parseOk("$artist in Bach"));
    }
  }

  TEST_CASE("ExecutionPlan - compiles in ranges", "[query][unit][execution-plan][membership]")
  {
    SECTION("CompilesRangeAsClosedBounds")
    {
      auto const plan = compileOk(parseOk("$year in 1990..1999"));

      checkClosedRange(plan, Field::Year, 1990, 1999);
    }

    SECTION("ScalesDurationRangeBounds")
    {
      auto const plan = compileOk(parseOk("@duration in 2m30s..5m"));

      checkClosedRange(plan, Field::Duration, 150000, 300000);
    }

    SECTION("RejectsStandaloneRange")
    {
      std::ignore = compileError(parseOk("1990..1999"));
    }

    SECTION("CompilesDictionaryRangeAsStringBounds")
    {
      // Dictionary fields hold interned IDs, so range bounds are kept as string
      // constants (not resolved to IDs) for lexicographic comparison at eval time.
      auto const plan = compileOk(parseOk("$artist in Bach..Mozart"));

      CHECK(plan.stringConstants == std::vector<std::string>{"Bach", "Mozart"});
      CHECK(plan.dictionarySymbols.empty());
      CHECK(plan.requiresDictionary);
      checkClosedRange(plan, Field::ArtistId, 0, 1);
    }

    SECTION("RejectsNonStringDictionaryRangeBounds")
    {
      // An ordered comparison over a dictionary field only makes sense against text.
      std::ignore = compileError(parseOk("$artist in 1..5"));
    }

    SECTION("AllowsRangeOnStringField")
    {
      // Plain string fields compare lexicographically, so a range is meaningful.
      auto const plan = compileOk(parseOk("$title in apple..zoo"));
      CHECK(plan.stringConstants == std::vector<std::string>{"apple", "zoo"});
      checkClosedRange(plan, Field::Title, 0, 1);
    }
  }

  TEST_CASE("ExecutionPlan - enforces ordered comparison field restrictions",
            "[query][unit][execution-plan][membership]")
  {
    SECTION("AllowsStringRelationalOnDictionaryField")
    {
      // Ordered comparisons over dictionary fields compare resolved text; the
      // operand is kept as a string constant rather than resolved to an ID.
      auto const plan = compileOk(parseOk("$artist > Bach"));

      REQUIRE(plan.stringConstants.size() == 1);
      CHECK(plan.stringConstants[0] == "Bach");
      CHECK(std::ranges::count(plan.instructions, OpCode::Gt, &Instruction::op) == 1);
    }

    SECTION("RejectsNonStringRelationalOnDictionaryField")
    {
      std::ignore = compileError(parseOk("$artist > 5"));
      std::ignore = compileError(parseOk("$genre <= 3m"));
    }

    SECTION("AllowsEqualityOnDictionaryField")
    {
      std::ignore = compileOk(parseOk("$artist = Bach"));
      std::ignore = compileOk(parseOk("$genre in [Classical, Jazz]"));
    }

    SECTION("AllowsRelationalOnNumericAndStringFields")
    {
      std::ignore = compileOk(parseOk("$year < 1990"));
      std::ignore = compileOk(parseOk("$title < zoo"));
      std::ignore = compileOk(parseOk("$coverArt > 0"));
    }
  }
} // namespace ao::query::test
