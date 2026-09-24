// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "test/unit/query/ExecutionPlanTestSupport.h"
#include <ao/AudioCodec.h>
#include <ao/Error.h>
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
#include <variant>
#include <vector>

namespace ao::query::test
{
  TEST_CASE("ExecutionPlan - compiles simple expressions", "[query][unit][execution-plan]")
  {
    auto expr = parseOk("$artist = Bach");
    auto plan = compileOk(expr);

    CHECK(plan.dictionarySymbols == std::vector<std::string>{"Bach"});
    CHECK(plan.stringConstants.empty());
    CHECK(plan.requiresDictionary);
    CHECK(checkComparison(plan, OpCode::Eq, Field::ArtistId, 0, 0)->operand == 1);
    CHECK_FALSE(plan.matchesAll);
  }

  TEST_CASE("ExecutionPlan - compiles constant true expressions", "[query][unit][execution-plan]")
  {
    auto expr = parseOk("true");
    auto plan = compileOk(expr);

    // Constant truth must not depend on the optional matchesAll shortcut.
    auto const constant = findInstruction(plan, OpCode::LoadConstant);
    CHECK(constant->operand == 0);
    CHECK(constant->constValue == 1);
    CHECK(plan.accessProfile == AccessProfile::NoTrackData);
    CHECK_FALSE(plan.requiresDictionary);
  }

  TEST_CASE("ExecutionPlan - compiles metadata fields", "[query][unit][execution-plan]")
  {
    auto expr = parseOk("$title = 'Test'");
    auto plan = compileOk(expr);

    CHECK(plan.stringConstants == std::vector<std::string>{"Test"});
    CHECK(plan.dictionarySymbols.empty());
    CHECK(checkComparison(plan, OpCode::Eq, Field::Title, 0)->operand == 1);
  }

  TEST_CASE("ExecutionPlan - compiles property fields", "[query][unit][execution-plan]")
  {
    auto expr = parseOk("@duration > 180000");
    auto plan = compileOk(expr);

    CHECK(checkComparison(plan, OpCode::Gt, Field::Duration, 180000)->operand == 1);
  }

  TEST_CASE("ExecutionPlan - compiles codec constants", "[query][unit][execution-plan]")
  {
    SECTION("WAV")
    {
      auto plan = compileOk(parseOk("@codec = WAV"));

      auto it = std::ranges::find(plan.instructions, OpCode::LoadConstant, &Instruction::op);

      REQUIRE(it != plan.instructions.end());
      CHECK(std::cmp_equal(it->constValue, audioCodecStorageValue(AudioCodec::Wav)));
    }

    SECTION("Opus")
    {
      auto plan = compileOk(parseOk("@codec = Opus"));

      auto it = std::ranges::find(plan.instructions, OpCode::LoadConstant, &Instruction::op);

      REQUIRE(it != plan.instructions.end());
      CHECK(std::cmp_equal(it->constValue, audioCodecStorageValue(AudioCodec::Opus)));
    }
  }

  TEST_CASE("ExecutionPlan - rejects unsupported codec constants", "[query][unit][execution-plan]")
  {
    std::ignore = compileError(parseOk("@codec = VORBIS"));
  }

  TEST_CASE("ExecutionPlan - compiles logical and", "[query][unit][execution-plan]")
  {
    // Use && for logical and to ensure it's parsed correctly
    auto expr = parseOk("$artist = Bach && $genre = Classical");
    auto plan = compileOk(expr);

    CHECK(plan.dictionarySymbols == std::vector<std::string>{"Bach", "Classical"});
    auto const artist = checkComparison(plan, OpCode::Eq, Field::ArtistId, 0, 0);
    auto const genre = checkComparison(plan, OpCode::Eq, Field::GenreId, 0, 1, 1);
    auto const conjunction = findInstruction(plan, OpCode::And);
    CHECK(artist < genre);
    CHECK(genre < conjunction);
    CHECK(artist->operand == 1);
    CHECK(genre->operand == artist->operand + 1);
    CHECK(conjunction->operand == genre->operand - 1);
  }

  TEST_CASE("ExecutionPlan - compiles logical or", "[query][unit][execution-plan]")
  {
    // Use || for logical or to ensure it's parsed correctly
    auto expr = parseOk("$artist = Bach || $artist = Mozart");
    auto plan = compileOk(expr);

    CHECK(plan.dictionarySymbols == std::vector<std::string>{"Bach", "Mozart"});
    auto const bach = checkComparison(plan, OpCode::Eq, Field::ArtistId, 0, 0);
    auto const mozart = checkComparison(plan, OpCode::Eq, Field::ArtistId, 0, 1, 1);
    auto const disjunction = findInstruction(plan, OpCode::Or);
    CHECK(bach < mozart);
    CHECK(mozart < disjunction);
    CHECK(bach->operand == 1);
    CHECK(mozart->operand == bach->operand + 1);
    CHECK(disjunction->operand == mozart->operand - 1);
  }

  TEST_CASE("ExecutionPlan - compiles logical not", "[query][unit][execution-plan]")
  {
    auto expr = parseOk("not #favorite");
    auto plan = compileOk(expr);

    CHECK(plan.dictionarySymbols == std::vector<std::string>{"favorite"});
    auto const tag = checkComparison(plan, OpCode::Eq, Field::Tag, 0, 0);
    auto const negation = findInstruction(plan, OpCode::Not);
    CHECK(tag < negation);
    CHECK(tag->operand == 1);
    CHECK(negation->operand == tag->operand - 1);
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
  }

  TEST_CASE("ExecutionPlan - rejects bare non-tag predicate operands", "[query][unit][execution-plan]")
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

  TEST_CASE("ExecutionPlan - rejects existence on non-variable operands", "[query][unit][execution-plan]")
  {
    std::ignore = compileError(parseOk("($year = 1990)?"));
    std::ignore = compileError(parseOk("1990?"));
    std::ignore = compileError(parseOk(R"("Bach"?)"));
  }

  TEST_CASE("ExecutionPlan - accepts bare and negated tag predicates", "[query][unit][execution-plan]")
  {
    std::ignore = compileOk(parseOk("#favorite"));
    std::ignore = compileOk(parseOk("!#favorite"));
  }

  TEST_CASE("ExecutionPlan - compiles relational operators", "[query][unit][execution-plan]")
  {
    auto expr = parseOk("$year < 2000");
    auto plan = compileOk(expr);

    CHECK(checkComparison(plan, OpCode::Lt, Field::Year, 2000)->operand == 1);

    expr = parseOk("$year <= 2000");
    plan = compileOk(expr);

    CHECK(checkComparison(plan, OpCode::Le, Field::Year, 2000)->operand == 1);
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

    SECTION("Unsupported variable type reaches field resolution in a comparison")
    {
      auto binaryPtr = std::make_unique<BinaryExpression>();
      binaryPtr->operand = VariableExpression{.type = VariableType::Metadata, .name = "year"};
      binaryPtr->optOperation =
        BinaryExpression::Operation{.op = Operator::Equal, .operand = ConstantExpression{std::int64_t{1990}}};
      auto expression = Expression{std::move(binaryPtr)};

      auto const validPlan = compileOk(expression);
      CHECK(checkComparison(validPlan, OpCode::Eq, Field::Year, 1990)->operand == 1);

      std::get<std::unique_ptr<BinaryExpression>>(expression)->operand =
        VariableExpression{.type = static_cast<VariableType>(99), .name = "year"};
      auto const error = compileError(expression);
      CHECK(error.code == Error::Code::FormatRejected);
      CHECK_THAT(error.message, Catch::Matchers::ContainsSubstring("unsupported variable type for 'year'"));
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
