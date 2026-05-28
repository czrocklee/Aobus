// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/track/TrackFilterResolver.h>

#include <catch2/catch_test_macros.hpp>

using namespace ao::uimodel::track;

namespace ao::uimodel::track::test
{
  TEST_CASE("TrackFilterResolver - Quick Search Terms", "[unit][uimodel][filter]")
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
    }
  }

  TEST_CASE("TrackFilterResolver - Complex Expressions", "[unit][uimodel][filter]")
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
} // namespace ao::uimodel::track::test
