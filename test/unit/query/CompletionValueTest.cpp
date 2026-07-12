// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/query/Completion.h>
#include <ao/query/Field.h>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <variant>

namespace ao::query::test
{
  namespace
  {
    QueryValueCompletion valueContext(std::optional<QueryCompletionAnalysis> optContext)
    {
      REQUIRE(optContext);
      auto const* context = std::get_if<QueryValueCompletion>(&*optContext);
      REQUIRE(context != nullptr);
      return *context;
    }
  } // namespace

  TEST_CASE("Completion - analyzes value context after complete operators", "[query][unit][completion]")
  {
    SECTION("Detects a normal comparison value")
    {
      auto const text = std::string{"$artist = Ma"};
      auto const context = valueContext(analyzeQueryCompletion(text, text.size()));

      CHECK(context.field == Field::ArtistId);
      CHECK(context.replacement.replaceBegin == text.find("Ma"));
      CHECK(context.replacement.replaceEnd == text.size());
      CHECK(context.replacement.prefix == "Ma");
    }

    SECTION("Detects an empty comparison value")
    {
      auto const text = std::string{"$artist ="};
      auto const context = valueContext(analyzeQueryCompletion(text, text.size()));

      CHECK(context.field == Field::ArtistId);
      CHECK(context.replacement.replaceBegin == text.size());
      CHECK(context.replacement.replaceEnd == text.size());
      CHECK(context.replacement.prefix.empty());
    }

    SECTION("Keeps in-list values bound to the left field")
    {
      auto const text = std::string{"$artist in [Ma"};
      auto const context = valueContext(analyzeQueryCompletion(text, text.size()));

      CHECK(context.field == Field::ArtistId);
      CHECK(context.replacement.replaceBegin == text.find("Ma"));
      CHECK(context.replacement.replaceEnd == text.size());
      CHECK(context.replacement.prefix == "Ma");
    }

    SECTION("Handles later values in an in-list")
    {
      auto const text = std::string{R"($artist in ["Miles", Mo)"};
      auto const context = valueContext(analyzeQueryCompletion(text, text.size()));

      CHECK(context.field == Field::ArtistId);
      CHECK(context.replacement.replaceBegin == text.find("Mo"));
      CHECK(context.replacement.replaceEnd == text.size());
      CHECK(context.replacement.prefix == "Mo");
    }

    SECTION("Handles quoted custom keys before values")
    {
      auto const text = std::string{R"(%"Mood" = Br)"};
      auto const context = valueContext(analyzeQueryCompletion(text, text.size()));

      CHECK(context.field == Field::Custom);
      CHECK(context.replacement.replaceBegin == text.find("Br"));
      CHECK(context.replacement.replaceEnd == text.size());
      CHECK(context.replacement.prefix == "Br");
    }

    SECTION("Ignores escaped quotes inside custom keys")
    {
      auto const text = std::string{R"(%"quote\"key" = Br)"};
      auto const context = valueContext(analyzeQueryCompletion(text, text.size()));

      CHECK(context.field == Field::Custom);
      CHECK(context.replacement.replaceBegin == text.find("Br"));
      CHECK(context.replacement.replaceEnd == text.size());
      CHECK(context.replacement.prefix == "Br");
    }
  }
} // namespace ao::query::test
