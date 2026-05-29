// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/query/Expression.h>
#include <ao/query/Parser.h>
#include <ao/query/Serializer.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace ao::query::test
{
  TEST_CASE("Serializer - Serializes Metadata Variable Prefix", "[query][unit][serializer]")
  {
    auto const var = VariableExpression{.type = VariableType::Metadata, .name = "artist"};
    CHECK(serialize(var) == "$artist");
  }

  TEST_CASE("Serializer - Serializes Property Variable Prefix", "[query][unit][serializer]")
  {
    auto const var = VariableExpression{.type = VariableType::Property, .name = "duration"};
    CHECK(serialize(var) == "@duration");
  }

  TEST_CASE("Serializer - Serializes Tag Variable Prefix", "[query][unit][serializer]")
  {
    auto const var = VariableExpression{.type = VariableType::Tag, .name = "rock"};
    CHECK(serialize(var) == "#rock");
  }

  TEST_CASE("Serializer - Serializes Custom Variable Prefix", "[query][unit][serializer]")
  {
    auto const var = VariableExpression{.type = VariableType::Custom, .name = "isrc"};
    CHECK(serialize(var) == "%isrc");
  }

  TEST_CASE("Serializer - Serializes Boolean Constant", "[query][unit][serializer]")
  {
    CHECK(serialize(ConstantExpression{true}) == "true");
    CHECK(serialize(ConstantExpression{false}) == "false");
  }

  TEST_CASE("Serializer - Serializes Integer Constant", "[query][unit][serializer]")
  {
    CHECK(serialize(ConstantExpression{std::int64_t{123}}) == "123");
    CHECK(serialize(ConstantExpression{std::int64_t{-7}}) == "-7");
  }

  TEST_CASE("Serializer - Serializes Unit Constant", "[query][unit][serializer]")
  {
    CHECK(serialize(ConstantExpression{UnitConstantExpression{"44.1k"}}) == "44.1k");
  }

  TEST_CASE("Serializer - Serializes String Constant With Quotes", "[query][unit][serializer]")
  {
    CHECK(serialize(ConstantExpression{std::string{"Bach"}}) == "\"Bach\"");
  }

  TEST_CASE("Serializer - Serializes Unary Not", "[query][unit][serializer]")
  {
    auto unaryPtr = std::make_unique<UnaryExpression>();
    unaryPtr->op = Operator::Not;
    unaryPtr->operand = VariableExpression{.type = VariableType::Metadata, .name = "artist"};
    CHECK(serialize(Expression{std::move(unaryPtr)}) == "not $artist");
  }

  TEST_CASE("Serializer - Serializes Each Binary Operator Token", "[query][unit][serializer]")
  {
    struct Case final
    {
      Operator op;
      std::string expected;
    };
    auto const cases = {Case{.op = Operator::And, .expected = " and "},
                        Case{.op = Operator::Or, .expected = " or "},
                        Case{.op = Operator::Less, .expected = " < "},
                        Case{.op = Operator::LessEqual, .expected = " <= "},
                        Case{.op = Operator::Greater, .expected = " > "},
                        Case{.op = Operator::GreaterEqual, .expected = " >= "},
                        Case{.op = Operator::Equal, .expected = " = "},
                        Case{.op = Operator::NotEqual, .expected = " != "},
                        Case{.op = Operator::Like, .expected = " ~ "},
                        Case{.op = Operator::Add, .expected = " + "}};

    for (auto const& c : cases)
    {
      auto binPtr = std::make_unique<BinaryExpression>();
      binPtr->operand = VariableExpression{.type = VariableType::Metadata, .name = "a"};
      binPtr->optOperation = BinaryExpression::Operation{
        .op = c.op, .operand = VariableExpression{.type = VariableType::Metadata, .name = "b"}};

      CHECK(serialize(Expression{std::move(binPtr)}).find(c.expected) != std::string::npos);
    }
  }

  TEST_CASE("Serializer - Parenthesizes Nested Binary Expressions", "[query][unit][serializer]")
  {
    // ($artist = "Bach") and ($year >= 2020)
    auto lhsPtr = std::make_unique<BinaryExpression>();
    lhsPtr->operand = VariableExpression{.type = VariableType::Metadata, .name = "artist"};
    lhsPtr->optOperation = BinaryExpression::Operation{.op = Operator::Equal, .operand = std::string{"Bach"}};

    auto rhsPtr = std::make_unique<BinaryExpression>();
    rhsPtr->operand = VariableExpression{.type = VariableType::Metadata, .name = "year"};
    rhsPtr->optOperation = BinaryExpression::Operation{.op = Operator::GreaterEqual, .operand = std::int64_t{2020}};

    auto rootPtr = std::make_unique<BinaryExpression>();
    rootPtr->operand = std::move(lhsPtr);
    rootPtr->optOperation = BinaryExpression::Operation{.op = Operator::And, .operand = std::move(rhsPtr)};

    auto result = serialize(Expression{std::move(rootPtr)});
    CHECK(result == "($artist = \"Bach\") and ($year >= 2020)");
  }

  TEST_CASE("Serializer - Does Not Parenthesize Root Binary Expression", "[query][unit][serializer]")
  {
    auto binPtr = std::make_unique<BinaryExpression>();
    binPtr->operand = VariableExpression{.type = VariableType::Metadata, .name = "a"};
    binPtr->optOperation = BinaryExpression::Operation{.op = Operator::Equal, .operand = std::int64_t{1}};
    CHECK(serialize(Expression{std::move(binPtr)}) == "$a = 1");
  }

  TEST_CASE("Serializer - RoundTrip ParseSerializeParse Preserves Canonical Shape", "[query][unit][serializer]")
  {
    auto queries = {R"($artist = "Bach" and $year >= 2020)",
                    "not ($year = 2020)",
                    R"($title ~ "Bach" or $composer ~ "Mozart")",
                    R"(%isrc = "X" and @duration >= 3m)"};

    for (auto const& q : queries)
    {
      auto const expr1 = parse(q);
      auto const s1 = serialize(expr1);
      auto const expr2 = parse(s1);
      auto const s2 = serialize(expr2);

      CHECK(s1 == s2);
    }
  }

  TEST_CASE("Serializer - Handles Empty Or Incomplete Expressions Defensively", "[query][unit][serializer]")
  {
    SECTION("Empty Expression")
    {
      // Default-constructed Expression is a Metadata variable with an empty name
      auto const empty = Expression{};
      CHECK(serialize(empty) == "$");
    }

    SECTION("Null Unary Operand")
    {
      auto unaryPtr = std::make_unique<UnaryExpression>();
      unaryPtr->op = Operator::Not;
      // unaryPtr->operand is default-constructed ($)
      CHECK(serialize(Expression{std::move(unaryPtr)}) == "not $");
    }

    SECTION("Null Binary Left Operand")
    {
      auto binPtr = std::make_unique<BinaryExpression>();
      // binPtr->operand is default-constructed ($)
      binPtr->optOperation = BinaryExpression::Operation{.op = Operator::And, .operand = ConstantExpression{true}};
      CHECK(serialize(Expression{std::move(binPtr)}) == "$ and true");
    }

    SECTION("Null Binary Right Operation")
    {
      auto binPtr = std::make_unique<BinaryExpression>();
      binPtr->operand = ConstantExpression{true};
      // binPtr->optOperation is nullopt
      CHECK(serialize(Expression{std::move(binPtr)}) == "true");
    }
  }
} // namespace ao::query::test
