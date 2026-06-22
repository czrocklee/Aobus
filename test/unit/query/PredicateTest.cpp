// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/query/Expression.h>
#include <ao/query/Parser.h>
#include <ao/query/Predicate.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string_view>
#include <utility>

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

  TEST_CASE("Predicate - Identifies Boolean Query Predicates", "[query][unit][predicate]")
  {
    auto const predicates = std::array{
      std::string_view{"#rock"},
      std::string_view{"$title?"},
      std::string_view{"@duration > 3m"},
      std::string_view{R"($artist = "Miles")"},
      std::string_view{"#rock and $title?"},
      std::string_view{"not #rock"},
      std::string_view{"true"},
      std::string_view{"false"},
    };

    for (auto const expression : predicates)
    {
      DYNAMIC_SECTION("Expression: " << expression)
      {
        CHECK(isPredicateExpression(parseOk(expression)));
      }
    }
  }

  TEST_CASE("Predicate - Rejects Non-Predicate Expressions", "[query][unit][predicate]")
  {
    auto const expressions = std::array{
      std::string_view{"$title"},
      std::string_view{"$artist"},
      std::string_view{"@duration"},
      std::string_view{"%rating"},
      std::string_view{"3m"},
      std::string_view{"1729"},
      std::string_view{R"("literal")"},
      std::string_view{R"($title + " - " + $artist)"},
    };

    for (auto const expression : expressions)
    {
      DYNAMIC_SECTION("Expression: " << expression)
      {
        CHECK_FALSE(isPredicateExpression(parseOk(expression)));
      }
    }
  }
} // namespace ao::query::test
