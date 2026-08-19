// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/query/ExecutionPlanTestSupport.h"
#include <ao/query/Field.h>
#include <ao/query/detail/Bytecode.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace ao::query::test
{
  namespace
  {
    Instruction const& findInstruction(ExecutionPlan const& plan, OpCode op)
    {
      auto const instruction = std::ranges::find(plan.instructions, op, &Instruction::op);
      REQUIRE(instruction != plan.instructions.end());
      return *instruction;
    }
  } // namespace

  TEST_CASE("ExecutionPlan - compiles custom field existence as an owned symbol", "[query][unit][execution-plan]")
  {
    auto const plan = compileOk(parseOk("%rating?"));

    REQUIRE(plan.instructions.size() == 1);
    REQUIRE(plan.dictionarySymbols == std::vector<std::string>{"rating"});
    CHECK(plan.instructions[0].op == OpCode::Exists);
    CHECK(plan.instructions[0].field == static_cast<std::uint8_t>(Field::Custom));
    CHECK(plan.instructions[0].dictionarySymbol == 0);
    CHECK(plan.requiresDictionary);
    CHECK(plan.accessProfile == AccessProfile::ColdOnly);
  }

  TEST_CASE("ExecutionPlan - dictionary-backed LIKE keeps a folded plan-owned key",
            "[query][unit][execution-plan][unicode]")
  {
    auto const plan = compileOk(parseOk(R"($artist ~ "Bach")"));

    REQUIRE(plan.stringConstants == std::vector<std::string>{"bach"});
    CHECK(plan.dictionarySymbols.empty());
    CHECK(findInstruction(plan, OpCode::Like).field == static_cast<std::uint8_t>(Field::ArtistId));
    CHECK(plan.requiresDictionary);
  }

  TEST_CASE("ExecutionPlan - dictionary-backed caseless LIKE keeps a folded plan-owned key",
            "[query][unit][execution-plan][unicode]")
  {
    auto const plan = compileOk(parseOk(R"($artist ~ "DVOŘÁK")"));

    REQUIRE(plan.stringConstants == std::vector<std::string>{"dvořák"});
    CHECK(plan.dictionarySymbols.empty());
    CHECK(findInstruction(plan, OpCode::Like).field == static_cast<std::uint8_t>(Field::ArtistId));
    CHECK(plan.requiresDictionary);
  }

  TEST_CASE("ExecutionPlan - unknown tags compile without dictionary mutation", "[query][unit][execution-plan]")
  {
    auto expression = parseOk("#FutureTag");
    auto const plan = compileOk(expression);

    REQUIRE(plan.dictionarySymbols == std::vector<std::string>{"FutureTag"});
    REQUIRE(plan.requiredTagSymbols == std::vector<std::uint32_t>{0});
    CHECK(findInstruction(plan, OpCode::Eq).dictionarySymbol == 0);
    CHECK(plan.requiresDictionary);
  }

  TEST_CASE("ExecutionPlan - unknown custom keys compile without dictionary mutation", "[query][unit][execution-plan]")
  {
    auto const plan = compileOk(parseOk("%FutureKey = 'Value'"));

    REQUIRE(plan.dictionarySymbols == std::vector<std::string>{"FutureKey"});
    auto const& load = findInstruction(plan, OpCode::LoadField);
    CHECK(load.field == static_cast<std::uint8_t>(Field::Custom));
    CHECK(load.dictionarySymbol == 0);
    CHECK(findInstruction(plan, OpCode::Eq).dictionarySymbol == 0);
  }

  TEST_CASE("ExecutionPlan - dictionary-backed equality compiles to a bindable symbol", "[query][unit][execution-plan]")
  {
    auto const plan = compileOk(parseOk("$artist = 'Bach'"));

    REQUIRE(plan.dictionarySymbols == std::vector<std::string>{"Bach"});
    CHECK(plan.stringConstants.empty());
    CHECK(findInstruction(plan, OpCode::Eq).dictionarySymbol == 0);
    CHECK(plan.requiresDictionary);
  }

  TEST_CASE("ExecutionPlan - canonicalizes Unicode lookup literals", "[query][unit][execution-plan][unicode]")
  {
    auto const dictionaryPlan = compileOk(parseOk("$artist = 'Dvor\u030Ca\u0301k'"));
    auto const stringPlan = compileOk(parseOk("$title ~ 'Cafe\u0301'"));

    REQUIRE(dictionaryPlan.dictionarySymbols == std::vector<std::string>{"Dvořák"});
    REQUIRE(stringPlan.stringConstants == std::vector<std::string>{"café"});
  }

  TEST_CASE("ExecutionPlan - rejects malformed UTF-8 text literals", "[query][unit][execution-plan][unicode]")
  {
    auto compileMalformed = [](Operator op)
    {
      auto binary = std::make_unique<BinaryExpression>();
      binary->operand = VariableExpression{.type = VariableType::Metadata, .name = "title"};
      binary->optOperation = BinaryExpression::Operation{
        .op = op,
        .operand = ConstantExpression{std::string{"\xC0\xAF", 2}},
      };
      return compileError(Expression{std::move(binary)});
    };

    auto const equalityError = compileMalformed(Operator::Equal);
    CHECK(equalityError.code == Error::Code::FormatRejected);
    CHECK(equalityError.message.contains("Query string literal"));

    auto const caselessError = compileMalformed(Operator::Like);
    CHECK(caselessError.code == Error::Code::FormatRejected);
    CHECK(caselessError.message.contains("Unicode caseless query literal"));
  }
} // namespace ao::query::test
