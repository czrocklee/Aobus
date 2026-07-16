// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/query/Parser.h>
#include <ao/query/Serializer.h>
#include <ao/uimodel/library/track/TrackFilterResolver.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace ao::uimodel;

namespace ao::uimodel::test
{
  TEST_CASE("TrackFilterResolver - resolves quick search terms", "[uimodel][unit][filter]")
  {
    SECTION("Empty filter")
    {
      auto const resolved = resolveTrackFilterExpression("");
      CHECK(resolved.mode == TrackFilterMode::None);
      CHECK(resolved.expression.empty());
    }

    SECTION("Single term")
    {
      auto const resolved = resolveTrackFilterExpression("beatles");
      CHECK(resolved.mode == TrackFilterMode::Quick);
      CHECK(resolved.expression ==
            "($title ~ \"beatles\" or $artist ~ \"beatles\" or $album ~ \"beatles\" or $albumArtist ~ \"beatles\" "
            "or $genre ~ \"beatles\" or $composer ~ \"beatles\" or $work ~ \"beatles\" or #beatles)");
    }

    SECTION("Multiple terms")
    {
      auto const resolved = resolveTrackFilterExpression("beatles help");
      CHECK(resolved.mode == TrackFilterMode::Quick);
      CHECK(resolved.expression.contains(") and ("));
    }

    SECTION("Quoted terms")
    {
      auto const resolved = resolveTrackFilterExpression("\"the beatles\"");
      CHECK(resolved.mode == TrackFilterMode::Quick);
      CHECK(resolved.expression.contains("\"the beatles\""));
      CHECK(resolved.expression.contains(R"(#"the beatles")"));
    }

    SECTION("Numeric terms are valid tag names")
    {
      auto const resolved = resolveTrackFilterExpression("123");
      CHECK(resolved.mode == TrackFilterMode::Quick);
      CHECK(resolved.expression.contains("or #123"));
    }
  }

  TEST_CASE("TrackFilterResolver - resolves complex expressions", "[uimodel][unit][filter]")
  {
    SECTION("Expression starting with $")
    {
      auto const resolved = resolveTrackFilterExpression("$year > 2000");
      CHECK(resolved.mode == TrackFilterMode::Expression);
      CHECK(resolved.expression == "$year > 2000");
    }

    SECTION("Expression starting with @")
    {
      auto const resolved = resolveTrackFilterExpression("@jazz");
      CHECK(resolved.mode == TrackFilterMode::Expression);
      CHECK(resolved.expression == "@jazz");
    }

    SECTION("Expression starting after parentheses and unary negation")
    {
      for (auto const* const expression : {"($year > 2000)", "not $year?", "NOT ($year?)", "!$year?"})
      {
        CHECK(resolveTrackFilterExpression(expression).mode == TrackFilterMode::Expression);
      }
    }
  }

  TEST_CASE("TrackFilterResolver - punctuation inside plain text remains a Quick filter",
            "[uimodel][unit][filter][regression]")
  {
    for (auto const* const filter : {"P!nk", "Live (1999)", "A+B", "rock $year"})
    {
      INFO(filter);
      CHECK(resolveTrackFilterExpression(filter).mode == TrackFilterMode::Quick);
    }
  }

  TEST_CASE("TrackFilterResolver - decodes serialized Quick-filter terms without losing escapes",
            "[uimodel][unit][filter][escaping]")
  {
    auto const value = std::string{R"(C:\Music "Live")"};
    auto const serialized = query::serialize(query::ConstantExpression{value});
    auto const resolved = resolveTrackFilterExpression(serialized);

    CHECK(resolved.mode == TrackFilterMode::Quick);
    CHECK(resolved.expression.contains(serialized));
    CHECK(query::parse(resolved.expression).has_value());
  }

  TEST_CASE("TrackFilterResolver - preserves quick terms containing both quote styles", "[uimodel][regression][filter]")
  {
    auto const resolved = resolveTrackFilterExpression(R"FILTER("a'b"'"')FILTER");

    CHECK(resolved.mode == TrackFilterMode::Quick);
    CHECK(resolved.expression.contains(R"FILTER($title ~ "a'b\"")FILTER"));
    CHECK(resolved.expression.contains(R"FILTER(#"a'b\"")FILTER"));
    CHECK_FALSE(resolved.expression.contains(R"FILTER($title ~ "a'b'")FILTER"));
    CHECK(query::parse(resolved.expression).has_value());
  }
} // namespace ao::uimodel::test
