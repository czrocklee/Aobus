// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/query/Parser.h>
#include <ao/query/Serializer.h>
#include <catch2/catch_test_macros.hpp>
#include <memory>

namespace ao::query::test
{
  TEST_CASE("Serializer - Serializes Metadata Variable Prefix")
  {
    auto const var = VariableExpression{.type = VariableType::Metadata, .name = "artist"};
    CHECK(serialize(var) == "$artist");
  }

  TEST_CASE("Serializer - Serializes Property Variable Prefix")
  {
    auto const var = VariableExpression{.type = VariableType::Property, .name = "duration"};
    CHECK(serialize(var) == "@duration");
  }

  TEST_CASE("Serializer - Serializes Tag Variable Prefix")
  {
    auto const var = VariableExpression{.type = VariableType::Tag, .name = "rock"};
    CHECK(serialize(var) == "#rock");
  }

  TEST_CASE("Serializer - Serializes Custom Variable Prefix")
  {
    auto const var = VariableExpression{.type = VariableType::Custom, .name = "isrc"};
    CHECK(serialize(var) == "%isrc");
  }

  TEST_CASE("Serializer - Serializes Boolean Constant")
  {
    CHECK(serialize(ConstantExpression{true}) == "true");
    CHECK(serialize(ConstantExpression{false}) == "false");
  }

  TEST_CASE("Serializer - Serializes Integer Constant")
  {
    CHECK(serialize(ConstantExpression{std::int64_t{123}}) == "123");
    CHECK(serialize(ConstantExpression{std::int64_t{-7}}) == "-7");
  }

  TEST_CASE("Serializer - Serializes Unit Constant")
  {
    CHECK(serialize(ConstantExpression{UnitConstantExpression{"44.1k"}}) == "44.1k");
  }

  TEST_CASE("Serializer - Serializes String Constant With Quotes")
  {
    CHECK(serialize(ConstantExpression{std::string{"Bach"}}) == "\"Bach\"");
  }

  TEST_CASE("Serializer - Serializes Unary Not")
  {
    auto unary = std::make_unique<UnaryExpression>();
    unary->op = Operator::Not;
    unary->operand = VariableExpression{.type = VariableType::Metadata, .name = "artist"};
    CHECK(serialize(Expression{std::move(unary)}) == "not $artist");
  }

  TEST_CASE("Serializer - Serializes Each Binary Operator Token")
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
      auto bin = std::make_unique<BinaryExpression>();
      bin->operand = VariableExpression{.type = VariableType::Metadata, .name = "a"};
      bin->optOperation = BinaryExpression::Operation{
        .op = c.op, .operand = VariableExpression{.type = VariableType::Metadata, .name = "b"}};

      CHECK(serialize(Expression{std::move(bin)}).find(c.expected) != std::string::npos);
    }
  }

  TEST_CASE("Serializer - Parenthesizes Nested Binary Expressions")
  {
    // ($artist = "Bach") and ($year >= 2020)
    auto lhs = std::make_unique<BinaryExpression>();
    lhs->operand = VariableExpression{.type = VariableType::Metadata, .name = "artist"};
    lhs->optOperation = BinaryExpression::Operation{.op = Operator::Equal, .operand = std::string{"Bach"}};

    auto rhs = std::make_unique<BinaryExpression>();
    rhs->operand = VariableExpression{.type = VariableType::Metadata, .name = "year"};
    rhs->optOperation = BinaryExpression::Operation{.op = Operator::GreaterEqual, .operand = std::int64_t{2020}};

    auto root = std::make_unique<BinaryExpression>();
    root->operand = std::move(lhs);
    root->optOperation = BinaryExpression::Operation{.op = Operator::And, .operand = std::move(rhs)};

    auto result = serialize(Expression{std::move(root)});
    CHECK(result == "($artist = \"Bach\") and ($year >= 2020)");
  }

  TEST_CASE("Serializer - Does Not Parenthesize Root Binary Expression")
  {
    auto bin = std::make_unique<BinaryExpression>();
    bin->operand = VariableExpression{.type = VariableType::Metadata, .name = "a"};
    bin->optOperation = BinaryExpression::Operation{.op = Operator::Equal, .operand = std::int64_t{1}};
    CHECK(serialize(Expression{std::move(bin)}) == "$a = 1");
  }

  TEST_CASE("Serializer - RoundTrip ParseSerializeParse Preserves Canonical Shape")
  {
    auto queries = {"$artist = \"Bach\" and $year >= 2020",
                    "not ($year = 2020)",
                    "$title ~ \"Bach\" or $composer ~ \"Mozart\"",
                    "%isrc = \"X\" and @duration >= 3m"};

    for (auto const& q : queries)
    {
      auto const expr1 = parse(q);
      auto const s1 = serialize(expr1);
      auto const expr2 = parse(s1);
      auto const s2 = serialize(expr2);

      CHECK(s1 == s2);
    }
  }

  TEST_CASE("Serializer - Handles Empty Or Incomplete Expressions Defensively")
  {
    SECTION("Empty Expression")
    {
      // Default-constructed Expression is a Metadata variable with an empty name
      auto const empty = Expression{};
      CHECK(serialize(empty) == "$");
    }

    SECTION("Null Unary Operand")
    {
      auto unary = std::make_unique<UnaryExpression>();
      unary->op = Operator::Not;
      // unary->operand is default-constructed ($)
      CHECK(serialize(Expression{std::move(unary)}) == "not $");
    }

    SECTION("Null Binary Left Operand")
    {
      auto bin = std::make_unique<BinaryExpression>();
      // bin->operand is default-constructed ($)
      bin->optOperation = BinaryExpression::Operation{.op = Operator::And, .operand = ConstantExpression{true}};
      CHECK(serialize(Expression{std::move(bin)}) == "$ and true");
    }

    SECTION("Null Binary Right Operation")
    {
      auto bin = std::make_unique<BinaryExpression>();
      bin->operand = ConstantExpression{true};
      // bin->optOperation is nullopt
      CHECK(serialize(Expression{std::move(bin)}) == "true");
    }
  }
} // namespace ao::query::test
