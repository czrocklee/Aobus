// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "test/unit/query/ExecutionPlanTestSupport.h"
#include <ao/AudioCodec.h>
#include <ao/query/Expression.h>
#include <ao/query/Field.h>
#include <ao/query/detail/Bytecode.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <tuple>
#include <utility>

namespace ao::query::test
{
  TEST_CASE("ExecutionPlan - compiles simple expressions", "[query][unit][execution-plan]")
  {
    auto expr = parseOk("$artist = Bach");
    auto plan = compileOk(expr);

    CHECK_FALSE(plan.instructions.empty());
    CHECK_FALSE(plan.matchesAll);
  }

  TEST_CASE("ExecutionPlan - compiles constant true expressions", "[query][unit][execution-plan]")
  {
    // Note: matchesAll is not automatically set - it's a hint for optimization
    // The plan should still compile a constant true expression
    auto expr = parseOk("true");
    auto plan = compileOk(expr);

    // The plan should have at least one instruction (LoadConstant)
    CHECK_FALSE(plan.instructions.empty());
  }

  TEST_CASE("ExecutionPlan - compiles metadata fields", "[query][unit][execution-plan]")
  {
    auto expr = parseOk("$title = 'Test'");
    auto plan = compileOk(expr);

    CHECK(plan.instructions.size() >= 2);
    CHECK(plan.instructions[0].op == OpCode::LoadField);

    bool hasEq = false;

    for (auto const& instr : plan.instructions)
    {
      if (instr.op == OpCode::Eq)
      {
        hasEq = true;
        break;
      }
    }

    CHECK(hasEq == true);
  }

  TEST_CASE("ExecutionPlan - compiles property fields", "[query][unit][execution-plan]")
  {
    auto expr = parseOk("@duration > 180000");
    auto plan = compileOk(expr);

    CHECK(plan.instructions.size() >= 2);
    CHECK(plan.instructions[0].op == OpCode::LoadField);

    bool hasGt = false;

    for (auto const& instr : plan.instructions)
    {
      if (instr.op == OpCode::Gt)
      {
        hasGt = true;
        break;
      }
    }

    CHECK(hasGt == true);
  }

  TEST_CASE("ExecutionPlan - compiles codec constants", "[query][unit][execution-plan]")
  {
    auto plan = compileOk(parseOk("@codec = WAV"));

    auto it = std::ranges::find(plan.instructions, OpCode::LoadConstant, &Instruction::op);

    REQUIRE(it != plan.instructions.end());
    CHECK(std::cmp_equal(it->constValue, audioCodecStorageValue(AudioCodec::Wav)));
  }

  TEST_CASE("ExecutionPlan - rejects unsupported codec constants", "[query][unit][execution-plan]")
  {
    std::ignore = compileError(parseOk("@codec = OPUS"));
  }

  TEST_CASE("ExecutionPlan - compiles logical and", "[query][unit][execution-plan]")
  {
    // Use && for logical and to ensure it's parsed correctly
    auto expr = parseOk("$artist = Bach && $genre = Classical");
    auto plan = compileOk(expr);

    bool hasAnd = false;

    for (auto const& instr : plan.instructions)
    {
      if (instr.op == OpCode::And)
      {
        hasAnd = true;
        break;
      }
    }

    CHECK(hasAnd == true);
  }

  TEST_CASE("ExecutionPlan - compiles logical or", "[query][unit][execution-plan]")
  {
    // Use || for logical or to ensure it's parsed correctly
    auto expr = parseOk("$artist = Bach || $artist = Mozart");
    auto plan = compileOk(expr);

    bool hasOr = false;

    for (auto const& instr : plan.instructions)
    {
      if (instr.op == OpCode::Or)
      {
        hasOr = true;
        break;
      }
    }

    CHECK(hasOr == true);
  }

  TEST_CASE("ExecutionPlan - compiles logical not", "[query][unit][execution-plan]")
  {
    auto expr = parseOk("not #favorite");
    auto plan = compileOk(expr);

    bool hasNot = false;

    for (auto const& instr : plan.instructions)
    {
      if (instr.op == OpCode::Not)
      {
        hasNot = true;
        break;
      }
    }

    CHECK(hasNot == true);
  }

  TEST_CASE("ExecutionPlan - compiles existence tests", "[query][unit][execution-plan]")
  {
    SECTION("FieldExistenceEmitsExistsOpcode")
    {
      auto const plan = compileOk(parseOk("$year?"));

      REQUIRE(plan.instructions.size() == 1);
      CHECK(plan.instructions[0].op == OpCode::Exists);
      CHECK(plan.instructions[0].field == static_cast<std::uint8_t>(Field::Year));
      CHECK(plan.accessProfile == AccessProfile::HotOnly);
    }

    SECTION("ColdFieldExistenceUpdatesAccessProfile")
    {
      auto const plan = compileOk(parseOk("@duration?"));

      REQUIRE(plan.instructions.size() == 1);
      CHECK(plan.instructions[0].op == OpCode::Exists);
      CHECK(plan.instructions[0].field == static_cast<std::uint8_t>(Field::Duration));
      CHECK(plan.accessProfile == AccessProfile::ColdOnly);
    }

    SECTION("BareNonTagVariablesAreRejectedAsPredicates")
    {
      std::ignore = compileError(parseOk("$year"));
      std::ignore = compileError(parseOk("@duration"));
      std::ignore = compileError(parseOk("%rating"));
      std::ignore = compileError(parseOk("not $year"));
      CHECK_THAT(compileError(parseOk("!$year")).message, Catch::Matchers::ContainsSubstring("!$year?"));
      std::ignore = compileError(parseOk("$artist and $year = 1990"));
      std::ignore = compileError(parseOk("$artist or $year = 1990"));
      std::ignore = compileError(parseOk("$year = 1990 or $artist"));
    }

    SECTION("ExistenceRequiresVariableOperand")
    {
      std::ignore = compileError(parseOk("($year = 1990)?"));
      std::ignore = compileError(parseOk("1990?"));
      std::ignore = compileError(parseOk(R"("Bach"?)"));
    }

    SECTION("BareTagsRemainPredicates")
    {
      std::ignore = compileOk(parseOk("#favorite"));
      std::ignore = compileOk(parseOk("!#favorite"));
    }
  }

  TEST_CASE("ExecutionPlan - compiles relational operators", "[query][unit][execution-plan]")
  {
    auto expr = parseOk("$year < 2000");
    auto plan = compileOk(expr);

    bool hasLt = false;

    for (auto const& instr : plan.instructions)
    {
      if (instr.op == OpCode::Lt)
      {
        hasLt = true;
        break;
      }
    }

    CHECK(hasLt == true);

    expr = parseOk("$year <= 2000");
    plan = compileOk(expr);

    bool hasLe = false;

    for (auto const& instr : plan.instructions)
    {
      if (instr.op == OpCode::Le)
      {
        hasLe = true;
        break;
      }
    }

    CHECK(hasLe == true);
  }

  TEST_CASE("ExecutionPlan - rejects add operators", "[query][unit][execution-plan]")
  {
    auto expr = parseOk("$title + $artist");
    std::ignore = compileError(expr);
  }

  TEST_CASE("ExecutionPlan - compiles boolean false to constant zero", "[query][unit][execution-plan]")
  {
    auto expr = parseOk("false");
    auto plan = compileOk(expr);
    REQUIRE_FALSE(plan.instructions.empty());
    CHECK(plan.instructions[0].op == OpCode::LoadConstant);
    CHECK(plan.instructions[0].constValue == 0);
    CHECK_FALSE(plan.matchesAll);
  }

  TEST_CASE("ExecutionPlan - rejects invalid AST nodes", "[query][unit][execution-plan]")
  {
    SECTION("Unsupported variable type")
    {
      auto var = VariableExpression{.type = static_cast<VariableType>(99), .name = "invalid"};
      std::ignore = compileError(var);
    }

    SECTION("Unsupported operator in BinaryExpression")
    {
      auto binaryPtr = std::make_unique<BinaryExpression>();
      binaryPtr->operand = VariableExpression{.type = VariableType::Metadata, .name = "title"};
      binaryPtr->optOperation =
        BinaryExpression::Operation{.op = Operator::Add, .operand = ConstantExpression{std::int64_t{100}}};

      std::ignore = compileError(std::move(binaryPtr));

      auto binaryInvalidPtr = std::make_unique<BinaryExpression>();
      binaryInvalidPtr->operand = VariableExpression{.type = VariableType::Metadata, .name = "title"};
      binaryInvalidPtr->optOperation =
        BinaryExpression::Operation{.op = static_cast<Operator>(99), .operand = ConstantExpression{std::int64_t{100}}};

      std::ignore = compileError(std::move(binaryInvalidPtr));
    }

    SECTION("Compiler rejects unsupported unary operators")
    {
      auto unaryPtr = std::make_unique<UnaryExpression>();
      unaryPtr->op = Operator::Add; // Unsupported unary operator
      unaryPtr->operand = VariableExpression{.type = VariableType::Tag, .name = "rock"};

      std::ignore = compileError(std::move(unaryPtr));
    }
  }
} // namespace ao::query::test
