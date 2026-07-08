// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/library/track/TrackFilterResolver.h>

#include <catch2/catch_test_macros.hpp>

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
      CHECK(resolved.expression.contains("$title ~ \"beatles\""));
      CHECK(resolved.expression.contains("$artist ~ \"beatles\""));
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
  }
} // namespace ao::uimodel::test
