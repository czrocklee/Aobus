// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/query/Completion.h>
#include <ao/query/Parser.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace ao::query::test
{
  namespace
  {
    bool isLogicalOperatorContext(std::optional<QueryCompletionContext> const& optContext)
    {
      return optContext && std::holds_alternative<QueryLogicalOperatorCompletionContext>(*optContext);
    }

    QueryLogicalOperatorCompletionContext logicalOperatorContext(std::optional<QueryCompletionContext> optContext)
    {
      REQUIRE(optContext);
      auto const* context = std::get_if<QueryLogicalOperatorCompletionContext>(&*optContext);
      REQUIRE(context != nullptr);
      return *context;
    }
  } // namespace

  TEST_CASE("Completion - analyzes logical operator context after complete expressions", "[query][unit][completion]")
  {
    SECTION("Offers an empty logical operator prefix after a comparison")
    {
      auto const text = std::string{R"($artist = "Miles" )"};
      auto const context = logicalOperatorContext(analyzeCompletionContext(text, text.size()));

      CHECK(context.replacement.replaceBegin == text.find_last_of('"') + 1);
      CHECK(context.replacement.replaceEnd == text.size());
      CHECK(context.replacement.prefix.empty());
      CHECK(completeQueryLogicalOperator(context.replacement.prefix) ==
            std::vector<std::string_view>{"and", "or", "&&", "||"});
    }

    SECTION("Completes keyword logical operators after a typed prefix")
    {
      auto const text = std::string{R"($artist = "Miles" a)"};
      auto const context = logicalOperatorContext(analyzeCompletionContext(text, text.size()));

      CHECK(context.replacement.replaceBegin == text.find_last_of('"') + 1);
      CHECK(context.replacement.replaceEnd == text.size());
      CHECK(context.replacement.prefix == "a");
      CHECK(completeQueryLogicalOperator(context.replacement.prefix) == std::vector<std::string_view>{"and"});
    }

    SECTION("Completes symbolic logical operators after a typed prefix")
    {
      auto const text = std::string{R"($year >= 1999 &)"};
      auto const context = logicalOperatorContext(analyzeCompletionContext(text, text.size()));

      CHECK(context.replacement.replaceBegin == text.find("1999") + 4);
      CHECK(context.replacement.replaceEnd == text.size());
      CHECK(context.replacement.prefix == "&");
      CHECK(completeQueryLogicalOperator(context.replacement.prefix) == std::vector<std::string_view>{"&&"});
    }

    SECTION("Treats postfix exists as a complete expression")
    {
      auto const text = std::string{"$artist?"};
      auto const context = logicalOperatorContext(analyzeCompletionContext(text, text.size()));

      CHECK(context.replacement.replaceBegin == text.size());
      CHECK(context.replacement.replaceEnd == text.size());
      CHECK(context.replacement.prefix.empty());
    }

    SECTION("Treats bare tag variables as complete expressions")
    {
      auto const cases =
        std::array{std::string{"#rock "}, std::string{R"(#"90s Rock" )"}, std::string{R"(#["90s Rock"] )"}};

      for (auto const& text : cases)
      {
        DYNAMIC_SECTION("Text: " << text)
        {
          auto const context = logicalOperatorContext(analyzeCompletionContext(text, text.size()));

          CHECK(context.replacement.replaceBegin == text.size() - 1);
          CHECK(context.replacement.replaceEnd == text.size());
          CHECK(context.replacement.prefix.empty());
          CHECK(completeQueryLogicalOperator(context.replacement.prefix) ==
                std::vector<std::string_view>{"and", "or", "&&", "||"});
        }
      }
    }

    SECTION("Completes logical operator prefixes after bare tags")
    {
      auto const text = std::string{"#rock o"};
      auto const context = logicalOperatorContext(analyzeCompletionContext(text, text.size()));

      CHECK(context.replacement.replaceBegin == text.find(' '));
      CHECK(context.replacement.replaceEnd == text.size());
      CHECK(context.replacement.prefix == "o");
      CHECK(completeQueryLogicalOperator(context.replacement.prefix) == std::vector<std::string_view>{"or"});
    }

    SECTION("Does not offer logical operators after an incomplete comparison")
    {
      CHECK_FALSE(analyzeCompletionContext("$artist = a", 10));
    }

    SECTION("Completes symbolic logical operator from an Unknown ampersand tail")
    {
      auto const text = std::string{"$year >= 1999 &"};
      auto const context = logicalOperatorContext(analyzeCompletionContext(text, text.size()));

      CHECK(context.replacement.replaceBegin == text.find_last_of(' '));
      CHECK(context.replacement.replaceEnd == text.size());
      CHECK(context.replacement.prefix == "&");
      CHECK(completeQueryLogicalOperator(context.replacement.prefix) == std::vector<std::string_view>{"&&"});
    }
  }

  TEST_CASE("Completion - backward scanner tracks complete predicate boundaries", "[query][unit][completion]")
  {
    auto const checkCompletePredicateBoundary = [](std::string_view expression)
    {
      REQUIRE(parse(expression).has_value());

      auto const text = std::string{expression} + " ";
      auto const context = logicalOperatorContext(analyzeCompletionContext(text, text.size()));

      CHECK(context.replacement.replaceBegin == expression.size());
      CHECK(context.replacement.replaceEnd == text.size());
      CHECK(context.replacement.prefix.empty());
    };

    auto const completePredicates = std::array{
      std::string_view{R"($artist = "Miles")"},
      std::string_view{R"($artist != "Miles")"},
      std::string_view{"$title ~ Love"},
      std::string_view{"$year < 2000"},
      std::string_view{"$year <= 2000"},
      std::string_view{"$year > 1999"},
      std::string_view{"$year >= 1999"},
      std::string_view{R"($artist in ["Miles", "Monk"])"},
      std::string_view{"$year in 1990..1999"},
      std::string_view{"@duration in 2m30s..5m"},
      std::string_view{"$artist?"},
      std::string_view{R"(%["Replay Gain"]?)"},
      std::string_view{"#rock"},
      std::string_view{R"(#"90s Rock")"},
      std::string_view{R"(#["90s Rock"])"},
      std::string_view{R"(($artist = "Miles"))"},
      std::string_view{R"(($artist in ["Miles", "Monk"]))"},
      std::string_view{R"((($artist = "Miles") and ($album = "Kind of Blue")))"},
    };

    for (auto const expression : completePredicates)
    {
      DYNAMIC_SECTION("Expression: " << expression)
      {
        checkCompletePredicateBoundary(expression);
      }
    }
  }

  TEST_CASE("Completion - backward scanner does not promote incomplete expressions", "[query][unit][completion]")
  {
    auto const incompleteExpressions = std::array{
      std::string_view{"$artist ="},
      std::string_view{"$artist !="},
      std::string_view{"$title ~"},
      std::string_view{"$year <"},
      std::string_view{"$year <="},
      std::string_view{"$year >"},
      std::string_view{"$year >="},
      std::string_view{"$artist in"},
      std::string_view{"$artist in ["},
      std::string_view{R"($artist in ["Miles",)"},
      std::string_view{"$year in 1990.."},
      std::string_view{"@duration in 2m.."},
      std::string_view{R"(#"unterminated)"},
      std::string_view{R"(%["Replay Gain"?)"},
      std::string_view{R"(($artist = "Miles")"},
    };

    for (auto const expression : incompleteExpressions)
    {
      DYNAMIC_SECTION("Expression: " << expression)
      {
        REQUIRE_FALSE(parse(expression).has_value());

        auto const text = std::string{expression} + " ";
        CHECK_FALSE(isLogicalOperatorContext(analyzeCompletionContext(text, text.size())));
      }
    }
  }

  TEST_CASE("Completion - backward scanner does not promote non-predicate expressions", "[query][unit][completion]")
  {
    auto const nonPredicateExpressions = std::array{
      std::string_view{"$title"},
      std::string_view{"$artist"},
      std::string_view{"@duration"},
      std::string_view{"%rating"},
      std::string_view{"3m"},
      std::string_view{R"("literal")"},
      std::string_view{R"($title + " - " + $artist)"},
    };

    for (auto const expression : nonPredicateExpressions)
    {
      DYNAMIC_SECTION("Expression: " << expression)
      {
        REQUIRE(parse(expression).has_value());

        auto const text = std::string{expression} + " ";
        CHECK_FALSE(isLogicalOperatorContext(analyzeCompletionContext(text, text.size())));
      }
    }
  }

  TEST_CASE("Completion - backward scanner rejects invalid group boundaries", "[query][unit][completion]")
  {
    auto const invalidGroupedExpressions = std::array{
      std::string_view{"($artist =)"},
      std::string_view{"($year >)"},
      std::string_view{"(garbage +)"},
      std::string_view{R"(($artist in ["Miles",]))"},
    };

    for (auto const expression : invalidGroupedExpressions)
    {
      DYNAMIC_SECTION("Expression: " << expression)
      {
        REQUIRE_FALSE(parse(expression).has_value());

        auto const text = std::string{expression} + " ";
        CHECK_FALSE(analyzeCompletionContext(text, text.size()));
      }
    }
  }
} // namespace ao::query::test
