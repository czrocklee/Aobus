// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/query/Completion.h>
#include <ao/query/Parser.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace ao::query::test
{
  namespace
  {
    bool isLogicalOperatorContext(std::optional<QueryCompletionAnalysis> const& optContext)
    {
      return optContext && std::holds_alternative<QueryLogicalOperatorCompletion>(*optContext);
    }

    QueryLogicalOperatorCompletion logicalOperatorContext(std::optional<QueryCompletionAnalysis> optContext)
    {
      REQUIRE(optContext);
      auto const* context = std::get_if<QueryLogicalOperatorCompletion>(&*optContext);
      REQUIRE(context != nullptr);
      return *context;
    }
  } // namespace

  TEST_CASE("Completion - analyzes logical operator context after complete expressions")
  {
    SECTION("Offers an empty logical operator prefix after a comparison")
    {
      auto const text = std::string{R"($artist = "Miles" )"};
      auto const context = logicalOperatorContext(analyzeQueryCompletion(text, text.size()));

      CHECK(context.replacement.replaceBegin == text.find_last_of('"') + 1);
      CHECK(context.replacement.replaceEnd == text.size());
      CHECK(context.replacement.prefix.empty());
      CHECK(completeQueryLogicalOperator(context.replacement.prefix) ==
            std::vector<std::string_view>{"and", "or", "&&", "||"});
    }

    SECTION("Completes keyword logical operators after a typed prefix")
    {
      struct Case final
      {
        std::string_view text;
        std::string_view prefix;
        std::string_view candidate;
      };

      constexpr auto kCases = std::array{
        Case{.text = R"($artist = "Miles" a)", .prefix = "a", .candidate = "and"},
        Case{.text = R"($artist = "Miles" O)", .prefix = "O", .candidate = "or"},
      };

      for (auto const& testCase : kCases)
      {
        DYNAMIC_SECTION("Prefix: " << testCase.prefix)
        {
          auto const context = logicalOperatorContext(analyzeQueryCompletion(testCase.text, testCase.text.size()));

          CHECK(context.replacement.replaceBegin == 17);
          CHECK(context.replacement.replaceEnd == 19);
          CHECK(context.replacement.prefix == testCase.prefix);
          CHECK(completeQueryLogicalOperator(context.replacement.prefix) ==
                std::vector<std::string_view>{testCase.candidate});
        }
      }
    }

    SECTION("Completes symbolic logical operators after a typed prefix")
    {
      struct Case final
      {
        std::string_view text;
        std::string_view prefix;
        std::string_view candidate;
      };

      constexpr auto kCases = std::array{
        Case{.text = "$year >= 1999 &", .prefix = "&", .candidate = "&&"},
        Case{.text = "$year >= 1999 |", .prefix = "|", .candidate = "||"},
      };

      for (auto const& testCase : kCases)
      {
        DYNAMIC_SECTION("Prefix: " << testCase.prefix)
        {
          auto const context = logicalOperatorContext(analyzeQueryCompletion(testCase.text, testCase.text.size()));

          CHECK(context.replacement.replaceBegin == 13);
          CHECK(context.replacement.replaceEnd == 15);
          CHECK(context.replacement.prefix == testCase.prefix);
          CHECK(completeQueryLogicalOperator(context.replacement.prefix) ==
                std::vector<std::string_view>{testCase.candidate});
        }
      }
    }

    SECTION("Treats postfix exists as a complete expression")
    {
      auto const text = std::string{"$artist?"};
      auto const context = logicalOperatorContext(analyzeQueryCompletion(text, text.size()));

      CHECK(context.replacement.replaceBegin == text.size());
      CHECK(context.replacement.replaceEnd == text.size());
      CHECK(context.replacement.prefix.empty());
    }

    SECTION("Treats bare tag variables as complete expressions")
    {
      auto const cases = std::array{std::string{"#rock "},
                                    std::string{"  #title "},
                                    std::string{R"(#"90s Rock" )"},
                                    std::string{R"(#["90s Rock"] )"}};

      for (auto const& text : cases)
      {
        DYNAMIC_SECTION("Text: " << text)
        {
          auto const context = logicalOperatorContext(analyzeQueryCompletion(text, text.size()));

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
      auto const context = logicalOperatorContext(analyzeQueryCompletion(text, text.size()));

      CHECK(context.replacement.replaceBegin == text.find(' '));
      CHECK(context.replacement.replaceEnd == text.size());
      CHECK(context.replacement.prefix == "o");
      CHECK(completeQueryLogicalOperator(context.replacement.prefix) == std::vector<std::string_view>{"or"});
    }

    SECTION("Uses the complete predicate at a mid-input cursor despite an unfinished suffix")
    {
      auto const predicate = std::string_view{"$trackNumber = $trackTotal"};
      auto const text = std::string{predicate} + "   and $year in [";
      auto const cursor = predicate.size() + 1;
      auto const context = logicalOperatorContext(analyzeQueryCompletion(text, cursor));

      CHECK(context.replacement.replaceBegin == predicate.size());
      CHECK(context.replacement.replaceEnd == cursor);
      CHECK(context.replacement.prefix.empty());
    }
  }

  TEST_CASE("Completion - applies the query byte limit to a standalone tag predicate")
  {
    auto text = std::string{"#"} + std::string(65535, 'x') + " ";
    CHECK(isLogicalOperatorContext(analyzeQueryCompletion(text, text.size())));

    text.insert(1, 1, 'x');
    CHECK_FALSE(isLogicalOperatorContext(analyzeQueryCompletion(text, text.size())));
  }

  TEST_CASE("Completion - applies structural limits to the predicate before a logical operator prefix")
  {
    auto const checkBoundary = [](std::string_view admitted, std::string_view excessive)
    {
      auto const complete = [](std::string_view predicate)
      {
        auto const text = std::string{predicate} + " a $year in [";
        // The cursor is after the typed `a`, before the unfinished suffix.
        return analyzeQueryCompletion(text, predicate.size() + 2);
      };

      auto const context = logicalOperatorContext(complete(admitted));
      CHECK(context.replacement.replaceBegin == admitted.size());
      CHECK(context.replacement.replaceEnd == admitted.size() + 2);
      CHECK(context.replacement.prefix == "a");
      CHECK(completeQueryLogicalOperator(context.replacement.prefix) == std::vector<std::string_view>{"and"});
      CHECK_FALSE(isLogicalOperatorContext(complete(excessive)));
    };

    SECTION("64 nested groups are admitted but 65 are not")
    {
      auto const wrap = [](std::size_t depth) { return std::string(depth, '(') + "$year?" + std::string(depth, ')'); };

      checkBoundary(wrap(64), wrap(65));
    }

    SECTION("512 structural tokens are admitted but 513 are not")
    {
      auto conjunction = std::string{};

      // Each comparison and conjunction contributes four tokens and two operators.
      for (std::size_t index = 0; index < 127; ++index)
      {
        conjunction += "$year=1 and ";
      }

      // The final group adds four tokens, or five with negation, at depth one.
      checkBoundary(conjunction + "($year?)", conjunction + "(!$year?)");
    }
  }

  TEST_CASE("Completion - offers logical operators after complete predicates")
  {
    auto const checkCompletePredicateBoundary = [](std::string_view expression)
    {
      REQUIRE(parse(expression).has_value());

      auto const text = std::string{expression} + " ";
      auto const context = logicalOperatorContext(analyzeQueryCompletion(text, text.size()));

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
      std::string_view{"$trackNumber = $trackTotal"},
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

  TEST_CASE("Completion - does not offer logical operators after incomplete expressions")
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
      std::string_view{"and #rock"},
    };

    for (auto const expression : incompleteExpressions)
    {
      DYNAMIC_SECTION("Expression: " << expression)
      {
        REQUIRE_FALSE(parse(expression).has_value());

        auto const text = std::string{expression} + " ";
        CHECK_FALSE(isLogicalOperatorContext(analyzeQueryCompletion(text, text.size())));
      }
    }
  }

  TEST_CASE("Completion - does not offer logical operators after non-predicate expressions")
  {
    auto const nonPredicateExpressions = std::array{
      std::string_view{"$title"},
      std::string_view{"$artist"},
      std::string_view{"@duration"},
      std::string_view{"%rating"},
      std::string_view{"3m"},
      std::string_view{R"("literal")"},
      std::string_view{R"($title + " - " + $artist)"},
      std::string_view{"$title + #rock"},
    };

    for (auto const expression : nonPredicateExpressions)
    {
      DYNAMIC_SECTION("Expression: " << expression)
      {
        REQUIRE(parse(expression).has_value());

        auto const text = std::string{expression} + " ";
        CHECK_FALSE(isLogicalOperatorContext(analyzeQueryCompletion(text, text.size())));
      }
    }
  }

  TEST_CASE("Completion - rejects invalid group boundaries")
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
        CHECK_FALSE(analyzeQueryCompletion(text, text.size()));
      }
    }
  }
} // namespace ao::query::test
