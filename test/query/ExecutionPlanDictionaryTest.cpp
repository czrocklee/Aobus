// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/query/ExecutionPlanTestSupport.h"
#include <ao/query/Expression.h>
#include <ao/query/Field.h>
#include <ao/query/detail/Bytecode.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace ao::query::test
{
  TEST_CASE("ExecutionPlan - compiles custom field existence as an owned symbol")
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

  TEST_CASE("ExecutionPlan - dictionary-backed LIKE keeps a folded plan-owned key")
  {
    auto const plan = compileOk(parseOk(R"($artist ~ "Bach")"));

    REQUIRE(plan.stringConstants == std::vector<std::string>{"bach"});
    CHECK(plan.dictionarySymbols.empty());
    CHECK(findInstruction(plan, OpCode::Like)->field == static_cast<std::uint8_t>(Field::ArtistId));
    CHECK(plan.requiresDictionary);
  }

  TEST_CASE("ExecutionPlan - dictionary-backed caseless LIKE keeps a folded plan-owned key")
  {
    auto const plan = compileOk(parseOk(R"($artist ~ "DVOŘÁK")"));

    REQUIRE(plan.stringConstants == std::vector<std::string>{"dvořák"});
    CHECK(plan.dictionarySymbols.empty());
    CHECK(findInstruction(plan, OpCode::Like)->field == static_cast<std::uint8_t>(Field::ArtistId));
    CHECK(plan.requiresDictionary);
  }

  TEST_CASE("ExecutionPlan - unknown tags compile without dictionary mutation")
  {
    auto expression = parseOk("#FutureTag");
    auto const plan = compileOk(expression);

    REQUIRE(plan.dictionarySymbols == std::vector<std::string>{"FutureTag"});
    REQUIRE(plan.requiredTagSymbols == std::vector<std::uint32_t>{0});
    CHECK(findInstruction(plan, OpCode::Eq)->dictionarySymbol == 0);
    CHECK(plan.requiresDictionary);
  }

  TEST_CASE("ExecutionPlan - unknown custom keys compile without dictionary mutation")
  {
    auto const plan = compileOk(parseOk("%FutureKey = 'Value'"));

    REQUIRE(plan.dictionarySymbols == std::vector<std::string>{"FutureKey"});
    auto const& load = *findInstruction(plan, OpCode::LoadField);
    CHECK(load.field == static_cast<std::uint8_t>(Field::Custom));
    CHECK(load.dictionarySymbol == 0);
    CHECK(findInstruction(plan, OpCode::Eq)->dictionarySymbol == 0);
  }

  TEST_CASE("ExecutionPlan - dictionary-backed equality compiles to a bindable symbol")
  {
    for (auto const* query : {"$artist = 'Bach'", "'Bach' = $artist"})
    {
      CAPTURE(query);
      auto const plan = compileOk(parseOk(query));

      REQUIRE(plan.dictionarySymbols == std::vector<std::string>{"Bach"});
      CHECK(plan.stringConstants.empty());
      auto const& comparison = *findInstruction(plan, OpCode::Eq);
      CHECK(comparison.field == static_cast<std::uint8_t>(Field::ArtistId));
      CHECK(comparison.dictionarySymbol == 0);
      CHECK(plan.requiresDictionary);
    }
  }

  TEST_CASE("ExecutionPlan - dictionary field equality compares IDs")
  {
    for (auto const& [query, op] :
         {std::pair{"$artist = $album", OpCode::Eq}, std::pair{"$artist != $album", OpCode::Ne}})
    {
      CAPTURE(query);
      auto const plan = compileOk(parseOk(query));
      CHECK(std::ranges::find(plan.instructions, OpCode::CompareText, &Instruction::op) == plan.instructions.end());
      auto const& comparison = *findInstruction(plan, op);
      CHECK(comparison.field == static_cast<std::uint8_t>(Field::ArtistId));
      CHECK(comparison.dictionarySymbol == kNoDictionarySymbol);
    }
  }

  TEST_CASE("ExecutionPlan - text field comparisons resolve both operand fields")
  {
    auto const ordered = compileOk(parseOk("$artist < $album"));
    auto const& less = *findInstruction(ordered, OpCode::CompareText);
    CHECK(less.textRelation == OpCode::Lt);
    CHECK(less.field == static_cast<std::uint8_t>(Field::ArtistId));
    CHECK(less.rightTextField == Field::AlbumId);

    auto const mixed = compileOk(parseOk("$title = $artist"));
    auto const& equal = *findInstruction(mixed, OpCode::CompareText);
    CHECK(equal.textRelation == OpCode::Eq);
    CHECK(equal.field == static_cast<std::uint8_t>(Field::Title));
    CHECK(equal.rightTextField == Field::ArtistId);
  }

  TEST_CASE("ExecutionPlan - reversed text ordering reverses the relation")
  {
    auto const plan = compileOk(parseOk("'Mozart' < $artist"));

    REQUIRE(plan.stringConstants == std::vector<std::string>{"Mozart"});
    auto const& comparison = *findInstruction(plan, OpCode::Gt);
    CHECK(comparison.field == static_cast<std::uint8_t>(Field::ArtistId));
  }

  TEST_CASE("ExecutionPlan - literal text comparison marks both operands")
  {
    auto const plan = compileOk(parseOk("'Bach' = 'Mozart'"));
    auto const& comparison = *findInstruction(plan, OpCode::CompareText);
    CHECK(comparison.textRelation == OpCode::Eq);
    CHECK(comparison.field == static_cast<std::uint8_t>(kTextLiteralField));
    CHECK(comparison.rightTextField == kTextLiteralField);
  }

  TEST_CASE("ExecutionPlan - canonicalizes Unicode lookup literals")
  {
    auto const dictionaryPlan = compileOk(parseOk("$artist = 'Dvor\u030Ca\u0301k'"));
    auto const stringPlan = compileOk(parseOk("$title ~ 'Cafe\u0301'"));

    REQUIRE(dictionaryPlan.dictionarySymbols == std::vector<std::string>{"Dvořák"});
    REQUIRE(stringPlan.stringConstants == std::vector<std::string>{"café"});
  }

  TEST_CASE("ExecutionPlan - rejects malformed UTF-8 text literals")
  {
    auto compileMalformed = [](Operator op)
    {
      auto binaryPtr = std::make_unique<BinaryExpression>();
      binaryPtr->operand = VariableExpression{.type = VariableType::Metadata, .name = "title"};
      binaryPtr->optOperation = BinaryExpression::Operation{
        .op = op,
        .operand = ConstantExpression{std::string{"\xC0\xAF", 2}},
      };
      return compileError(Expression{std::move(binaryPtr)});
    };

    auto const equalityError = compileMalformed(Operator::Equal);
    CHECK(equalityError.code == Error::Code::FormatRejected);
    CHECK(equalityError.message.contains("Query string literal"));

    auto const caselessError = compileMalformed(Operator::Like);
    CHECK(caselessError.code == Error::Code::FormatRejected);
    CHECK(caselessError.message.contains("Unicode caseless query literal"));
  }
} // namespace ao::query::test
