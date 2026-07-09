// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/query/Expression.h>
#include <ao/query/Parser.h>
#include <ao/query/Serializer.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace ao::query::test
{
  namespace
  {
    Expression parseOk(std::string_view text)
    {
      auto result = ::ao::query::parse(text);
      REQUIRE(result.has_value());
      return std::move(*result);
    }
  } // namespace

  TEST_CASE("Serializer - serializes system variable prefixes", "[query][unit][serializer]")
  {
    CHECK(serialize(VariableExpression{.type = VariableType::Metadata, .name = "artist"}) == "$artist");
    CHECK(serialize(VariableExpression{.type = VariableType::Property, .name = "duration"}) == "@duration");
  }

  TEST_CASE("Serializer - serializes user variable names", "[query][unit][serializer]")
  {
    SECTION("Simple names stay bare")
    {
      CHECK(serialize(VariableExpression{.type = VariableType::Tag, .name = "rock"}) == "#rock");
      CHECK(serialize(VariableExpression{.type = VariableType::Custom, .name = "isrc"}) == "%isrc");
    }

    SECTION("Numeric names stay bare")
    {
      CHECK(serialize(VariableExpression{.type = VariableType::Tag, .name = "123"}) == "#123");
      CHECK(serialize(VariableExpression{.type = VariableType::Custom, .name = "123"}) == "%123");
    }

    SECTION("Complex names are quoted and escaped")
    {
      auto const escaped = VariableExpression{.type = VariableType::Custom, .name = "quote\"and\\slash"};

      CHECK(serialize(VariableExpression{.type = VariableType::Tag, .name = "90s Rock"}) == R"(#"90s Rock")");
      CHECK(serialize(escaped) == R"(%"quote\"and\\slash")");
      CHECK(serialize(VariableExpression{.type = VariableType::Custom, .name = "with\nnewline"}) ==
            R"(%"with\nnewline")");
    }

    SECTION("Quoted names round-trip")
    {
      auto const var = VariableExpression{.type = VariableType::Custom, .name = "Replay Gain"};
      CHECK(serialize(parseOk(serialize(var))) == serialize(var));
    }
  }

  TEST_CASE("Serializer - serializes boolean constants", "[query][unit][serializer]")
  {
    CHECK(serialize(ConstantExpression{true}) == "true");
    CHECK(serialize(ConstantExpression{false}) == "false");
  }

  TEST_CASE("Serializer - serializes integer constants", "[query][unit][serializer]")
  {
    CHECK(serialize(ConstantExpression{std::int64_t{123}}) == "123");
    CHECK(serialize(ConstantExpression{std::int64_t{-7}}) == "-7");
  }

  TEST_CASE("Serializer - serializes unit constants", "[query][unit][serializer]")
  {
    CHECK(serialize(ConstantExpression{UnitConstantExpression{"44.1k"}}) == "44.1k");
  }

  TEST_CASE("Serializer - serializes string constants with quotes", "[query][unit][serializer]")
  {
    CHECK(serialize(ConstantExpression{std::string{"Bach"}}) == "\"Bach\"");
    CHECK(serialize(ConstantExpression{std::string{"quote\"and\\slash"}}) == R"("quote\"and\\slash")");
    CHECK(serialize(ConstantExpression{std::string{"line1\nline2"}}) == R"("line1\nline2")");
    CHECK(serialize(ConstantExpression{std::string{"col1\tcol2"}}) == R"("col1\tcol2")");
    CHECK(serialize(ConstantExpression{std::string{"cr\rlf"}}) == R"("cr\rlf")");
    CHECK(serialize(ConstantExpression{std::string{"has'apostrophe"}}) == R"("has'apostrophe")");
  }

  TEST_CASE("Serializer - round-trips string constants with control characters", "[query][unit][serializer]")
  {
    auto const original = std::string{"quote\"\n\t\r\\slash"};
    auto const expr = ConstantExpression{original};
    auto const serialized = serialize(expr);
    auto const reparsed = parseOk(serialized);
    CHECK(serialize(reparsed) == serialized);

    auto const* constant = std::get_if<ConstantExpression>(&reparsed);
    REQUIRE(constant != nullptr);
    auto const* value = std::get_if<std::string>(constant);
    REQUIRE(value != nullptr);
    CHECK(*value == original);
  }

  TEST_CASE("Serializer - serializes unary not", "[query][unit][serializer]")
  {
    auto unaryPtr = std::make_unique<UnaryExpression>();
    unaryPtr->op = Operator::Not;
    unaryPtr->operand = VariableExpression{.type = VariableType::Metadata, .name = "artist"};
    CHECK(serialize(Expression{std::move(unaryPtr)}) == "not $artist");
  }

  TEST_CASE("Serializer - serializes existence tests", "[query][unit][serializer]")
  {
    CHECK(serialize(parseOk("$year?")) == "$year?");
    CHECK(serialize(parseOk("!$year?")) == "not $year?");
    CHECK(serialize(parseOk("not $composer?")) == "not $composer?");
    CHECK(serialize(parseOk("!@duration?")) == "not @duration?");
    CHECK(serialize(parseOk("!%rating?")) == "not %rating?");
    CHECK(serialize(parseOk("#favorite?")) == "#favorite?");
    CHECK(serialize(parseOk(R"(%"Replay Gain"?)")) == R"(%"Replay Gain"?)");

    auto binaryPtr = std::make_unique<BinaryExpression>();
    binaryPtr->operand = VariableExpression{.type = VariableType::Metadata, .name = "year"};
    binaryPtr->optOperation = BinaryExpression::Operation{.op = Operator::Equal, .operand = std::int64_t{1990}};

    auto unaryPtr = std::make_unique<UnaryExpression>();
    unaryPtr->op = Operator::Exists;
    unaryPtr->operand = std::move(binaryPtr);
    CHECK(serialize(Expression{std::move(unaryPtr)}) == "($year = 1990)?");
  }

  TEST_CASE("Serializer - serializes each binary operator token", "[query][unit][serializer]")
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
                        Case{.op = Operator::In, .expected = " in "},
                        Case{.op = Operator::Add, .expected = " + "}};

    for (auto const& c : cases)
    {
      auto binaryPtr = std::make_unique<BinaryExpression>();
      binaryPtr->operand = VariableExpression{.type = VariableType::Metadata, .name = "a"};
      binaryPtr->optOperation = BinaryExpression::Operation{
        .op = c.op, .operand = VariableExpression{.type = VariableType::Metadata, .name = "b"}};

      CHECK(serialize(Expression{std::move(binaryPtr)}).find(c.expected) != std::string::npos);
    }
  }

  TEST_CASE("Serializer - parenthesizes nested binary expressions", "[query][unit][serializer]")
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

  TEST_CASE("Serializer - omits parentheses around root binary expressions", "[query][unit][serializer]")
  {
    auto binaryPtr = std::make_unique<BinaryExpression>();
    binaryPtr->operand = VariableExpression{.type = VariableType::Metadata, .name = "a"};
    binaryPtr->optOperation = BinaryExpression::Operation{.op = Operator::Equal, .operand = std::int64_t{1}};
    CHECK(serialize(Expression{std::move(binaryPtr)}) == "$a = 1");
  }

  TEST_CASE("Serializer - serializes in lists", "[query][unit][serializer]")
  {
    auto binaryPtr = std::make_unique<BinaryExpression>();
    binaryPtr->operand = VariableExpression{.type = VariableType::Metadata, .name = "artist"};
    binaryPtr->optOperation = BinaryExpression::Operation{
      .op = Operator::In, .operand = ListExpression{.values = {std::string{"Bach"}, std::string{"Mozart"}}}};

    CHECK(serialize(Expression{std::move(binaryPtr)}) == R"($artist in ["Bach", "Mozart"])");
  }

  TEST_CASE("Serializer - serializes in ranges", "[query][unit][serializer]")
  {
    auto binaryPtr = std::make_unique<BinaryExpression>();
    binaryPtr->operand = VariableExpression{.type = VariableType::Property, .name = "duration"};
    binaryPtr->optOperation = BinaryExpression::Operation{
      .op = Operator::In,
      .operand = RangeExpression{.lower = UnitConstantExpression{"2m30s"}, .upper = UnitConstantExpression{"5m"}}};

    CHECK(serialize(Expression{std::move(binaryPtr)}) == "@duration in 2m30s..5m");
  }

  TEST_CASE("Serializer - preserves canonical shape across parse serialize parse", "[query][unit][serializer]")
  {
    auto queries = {R"($artist = "Bach" and $year >= 2020)",
                    "not ($year = 2020)",
                    R"($title ~ "Bach" or $composer ~ "Mozart")",
                    R"(%isrc = "X" and @duration >= 3m)",
                    R"($artist in ["Bach", "Mozart"])",
                    "@duration in 2m30s..5m",
                    R"(#"90s Rock" and %"Replay Gain" = "high")",
                    R"($year? and %"Replay Gain"?)",
                    R"($title = "A \"quote\"")"};

    for (auto const& q : queries)
    {
      auto const expr1 = parseOk(q);
      auto const s1 = serialize(expr1);
      auto const expr2 = parseOk(s1);
      auto const s2 = serialize(expr2);

      CHECK(s1 == s2);
    }
  }

  TEST_CASE("Serializer - handles empty or incomplete expressions defensively", "[query][unit][serializer]")
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
      auto binaryPtr = std::make_unique<BinaryExpression>();
      // binaryPtr->operand is default-constructed ($)
      binaryPtr->optOperation = BinaryExpression::Operation{.op = Operator::And, .operand = ConstantExpression{true}};
      CHECK(serialize(Expression{std::move(binaryPtr)}) == "$ and true");
    }

    SECTION("Null Binary Right Operation")
    {
      auto binaryPtr = std::make_unique<BinaryExpression>();
      binaryPtr->operand = ConstantExpression{true};
      // binaryPtr->optOperation is nullopt
      CHECK(serialize(Expression{std::move(binaryPtr)}) == "true");
    }
  }
} // namespace ao::query::test
