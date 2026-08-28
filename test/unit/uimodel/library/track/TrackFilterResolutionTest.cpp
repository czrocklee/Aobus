// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/query/Parser.h>
#include <ao/query/Serializer.h>
#include <ao/uimodel/library/track/TrackFilter.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace ao::uimodel;

namespace ao::uimodel::test
{
  TEST_CASE("TrackFilterResolution - resolves quick search terms", "[uimodel][unit][filter]")
  {
    SECTION("Empty filter")
    {
      auto const resolved = resolveTrackFilter("");
      CHECK(resolved.mode == TrackFilterMode::None);
      CHECK(resolved.expression.empty());
    }

    SECTION("Single term")
    {
      auto const resolved = resolveTrackFilter("beatles");
      CHECK(resolved.mode == TrackFilterMode::Quick);
      CHECK(resolved.expression ==
            "($title ~ \"beatles\" or $artist ~ \"beatles\" or $album ~ \"beatles\" or $albumArtist ~ "
            "\"beatles\" or $genre ~ \"beatles\" or $composer ~ \"beatles\" or $work ~ \"beatles\" or "
            "#beatles)");
    }

    SECTION("Multiple terms")
    {
      auto const resolved = resolveTrackFilter("beatles help");
      CHECK(resolved.mode == TrackFilterMode::Quick);
      CHECK(resolved.expression.contains(") and ("));
    }

    SECTION("Quoted terms")
    {
      auto const resolved = resolveTrackFilter("\"the beatles\"");
      CHECK(resolved.mode == TrackFilterMode::Quick);
      CHECK(resolved.expression.contains("\"the beatles\""));
      CHECK(resolved.expression.contains(R"(#"the beatles")"));
    }

    SECTION("Numeric terms are valid tag names")
    {
      auto const resolved = resolveTrackFilter("123");
      CHECK(resolved.mode == TrackFilterMode::Quick);
      CHECK(resolved.expression.contains("or #123"));
    }
  }

  TEST_CASE("TrackFilterResolution - resolves complex expressions", "[uimodel][unit][filter]")
  {
    SECTION("Expression starting with $")
    {
      auto const resolved = resolveTrackFilter("$year > 2000");
      CHECK(resolved.mode == TrackFilterMode::Expression);
      CHECK(resolved.expression == "$year > 2000");
    }

    SECTION("Expression starting with @")
    {
      auto const resolved = resolveTrackFilter("@jazz");
      CHECK(resolved.mode == TrackFilterMode::Expression);
      CHECK(resolved.expression == "@jazz");
    }

    SECTION("Expression starting after parentheses and unary negation")
    {
      for (auto const* const expression : {"($year > 2000)", "not $year?", "NOT ($year?)", "!$year?"})
      {
        CHECK(resolveTrackFilter(expression).mode == TrackFilterMode::Expression);
      }
    }
  }

  TEST_CASE("TrackFilterResolution - punctuation inside plain text remains a Quick filter",
            "[uimodel][unit][filter][regression]")
  {
    for (auto const* const filter : {"P!nk", "Live (1999)", "A+B", "rock $year"})
    {
      INFO(filter);
      CHECK(resolveTrackFilter(filter).mode == TrackFilterMode::Quick);
    }
  }

  TEST_CASE("TrackFilterResolution - decodes serialized Quick-filter terms without losing escapes",
            "[uimodel][unit][filter][escaping]")
  {
    auto const value = std::string{R"(C:\Music "Live")"};
    auto const serialized = query::serialize(query::ConstantExpression{value});
    auto const resolved = resolveTrackFilter(serialized);

    CHECK(resolved.mode == TrackFilterMode::Quick);
    CHECK(resolved.expression.contains(serialized));
    CHECK(query::parse(resolved.expression).has_value());
  }

  TEST_CASE("TrackFilterResolution - preserves quick terms containing both quote styles",
            "[uimodel][regression][filter]")
  {
    auto const resolved = resolveTrackFilter(R"FILTER("a'b"'"')FILTER");

    CHECK(resolved.mode == TrackFilterMode::Quick);
    CHECK(resolved.expression.contains(R"FILTER($title ~ "a'b\"")FILTER"));
    CHECK(resolved.expression.contains(R"FILTER(#"a'b\"")FILTER"));
    CHECK_FALSE(resolved.expression.contains(R"FILTER($title ~ "a'b'")FILTER"));
    CHECK(query::parse(resolved.expression).has_value());
  }
} // namespace ao::uimodel::test
