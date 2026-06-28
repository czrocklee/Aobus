// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/query/Completion.h>
#include <ao/query/Field.h>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace ao::query::test
{
  namespace
  {
    QueryOperatorCompletionContext operatorContext(std::optional<QueryCompletionContext> optContext)
    {
      REQUIRE(optContext);
      auto const* context = std::get_if<QueryOperatorCompletionContext>(&*optContext);
      REQUIRE(context != nullptr);
      return *context;
    }
  } // namespace

  TEST_CASE("Completion - analyzes operator context after query variables", "[query][unit][completion]")
  {
    SECTION("Offers an empty operator prefix after trailing whitespace")
    {
      auto const text = std::string{"$artist "};
      auto const context = operatorContext(analyzeCompletionContext(text, text.size()));

      CHECK(context.field == Field::ArtistId);
      CHECK(context.replacement.replaceBegin == 7);
      CHECK(context.replacement.replaceEnd == text.size());
      CHECK(context.replacement.prefix.empty());
      CHECK(completeQueryOperator(context.field, context.replacement.prefix) ==
            std::vector<std::string_view>{"=", "!=", "~", "in", "?"});
    }

    SECTION("Normalizes replacement over whitespace and typed symbol prefixes")
    {
      auto const text = std::string{"$year >"};
      auto const context = operatorContext(analyzeCompletionContext(text, text.size()));

      CHECK(context.field == Field::Year);
      CHECK(context.replacement.replaceBegin == 5);
      CHECK(context.replacement.replaceEnd == text.size());
      CHECK(context.replacement.prefix == ">");
      CHECK(completeQueryOperator(context.field, context.replacement.prefix) ==
            std::vector<std::string_view>{">", ">="});
    }

    SECTION("Completes keyword operators only after a variable boundary")
    {
      auto const text = std::string{"$artist i"};
      auto const context = operatorContext(analyzeCompletionContext(text, text.size()));

      CHECK(context.field == Field::ArtistId);
      CHECK(context.replacement.replaceBegin == 7);
      CHECK(context.replacement.replaceEnd == text.size());
      CHECK(context.replacement.prefix == "i");
      CHECK(completeQueryOperator(context.field, context.replacement.prefix) == std::vector<std::string_view>{"in"});

      auto optGlued = queryCompletionTokenAtCursor("$artistin", 9);
      REQUIRE(optGlued);
      CHECK(optGlued->prefix == "artistin");
      CHECK(completeQueryVariable(optGlued->type, optGlued->prefix).empty());
    }

    SECTION("Resolves quoted user variables as lvalues")
    {
      auto const text = std::string{R"(%"Mood" )"};
      auto const context = operatorContext(analyzeCompletionContext(text, text.size()));

      CHECK(context.field == Field::Custom);
      CHECK(context.replacement.replaceBegin == 7);
      CHECK(context.replacement.replaceEnd == text.size());
      CHECK(completeQueryOperator(context.field, context.replacement.prefix) ==
            std::vector<std::string_view>{"=", "!=", "~", "in", "?"});
    }

    SECTION("Resolves bracketed quoted user variables as lvalues")
    {
      auto const text = std::string{R"(%["Replay Gain"] )"};
      auto const context = operatorContext(analyzeCompletionContext(text, text.size()));

      CHECK(context.field == Field::Custom);
      CHECK(context.replacement.replaceBegin == text.size() - 1);
      CHECK(context.replacement.replaceEnd == text.size());
      CHECK(completeQueryOperator(context.field, context.replacement.prefix) ==
            std::vector<std::string_view>{"=", "!=", "~", "in", "?"});
    }

    SECTION("Resolves bracketed quoted user variables as lvalues without trailing space")
    {
      auto const text = std::string{R"(%["Replay Gain"])"};
      auto const context = operatorContext(analyzeCompletionContext(text, text.size()));

      CHECK(context.field == Field::Custom);
      CHECK(context.replacement.replaceBegin == text.size());
      CHECK(context.replacement.replaceEnd == text.size());
      CHECK(completeQueryOperator(context.field, context.replacement.prefix) ==
            std::vector<std::string_view>{"=", "!=", "~", "in", "?"});
    }

    SECTION("Rejects invalid escape in quoted variable as operator lvalue")
    {
      auto const text = std::string{R"(%"a\x"=)"};
      CHECK_FALSE(analyzeCompletionContext(text, text.size()));
    }
  }

  TEST_CASE("Completion - filters operator catalog by field kind", "[query][unit][completion]")
  {
    CHECK(completeQueryOperator(Field::ArtistId, "") == std::vector<std::string_view>{"=", "!=", "~", "in", "?"});
    CHECK(completeQueryOperator(Field::Year, "") ==
          std::vector<std::string_view>{"=", "!=", "<", "<=", ">", ">=", "in", "?"});
    CHECK(completeQueryOperator(Field::Tag, "") == std::vector<std::string_view>{"?"});
    CHECK(completeQueryOperator(Field::ArtistId, "!") == std::vector<std::string_view>{"!="});
  }
} // namespace ao::query::test
