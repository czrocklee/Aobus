// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/query/PlanEvaluatorTestSupport.h"
#include <ao/query/PlanEvaluator.h>
#include <ao/query/QueryCompiler.h>

#include <catch2/catch_test_macros.hpp>

namespace ao::query::test
{
  TEST_CASE("PlanEvaluator requires all AND operands to match", "[query][unit][plan_evaluator]")
  {
    auto expr = parseOk("$year = 2020 && @duration > 100000");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);
    auto evaluator = PlanEvaluator{};

    auto track1 = TestTrack{"Test", "Artist", "Album", "/path", 2020, 5, 180000};
    auto result = evaluator.evaluateFull(plan, track1.view());
    CHECK(result == true);

    auto track2 = TestTrack{"Test", "Artist", "Album", "/path", 2019, 5, 180000};
    result = evaluator.evaluateFull(plan, track2.view());
    CHECK(result == false);

    auto track3 = TestTrack{"Test", "Artist", "Album", "/path", 2020, 5, 50000};
    result = evaluator.evaluateFull(plan, track3.view());
    CHECK(result == false);
  }

  TEST_CASE("PlanEvaluator matches when any OR operand matches", "[query][unit][plan_evaluator]")
  {
    auto expr = parseOk("$year = 2020 || $year = 2019");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);
    auto evaluator = PlanEvaluator{};

    auto track1 = TestTrack{"Test", "Artist", "Album", "/path", 2020};
    auto result = evaluator.evaluateFull(plan, track1.view());
    CHECK(result == true);

    auto track2 = TestTrack{"Test", "Artist", "Album", "/path", 2019};
    result = evaluator.evaluateFull(plan, track2.view());
    CHECK(result == true);

    auto track3 = TestTrack{"Test", "Artist", "Album", "/path", 2018};
    result = evaluator.evaluateFull(plan, track3.view());
    CHECK(result == false);
  }

  TEST_CASE("PlanEvaluator negates grouped expressions", "[query][unit][plan_evaluator]")
  {
    // Use "not(" for explicit grouping, or check if parser handles precedence
    auto expr = parseOk("!($year = 2020)");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);
    auto evaluator = PlanEvaluator{};

    auto track1 = TestTrack{"Test", "Artist", "Album", "/path", 2020};
    auto result = evaluator.evaluateFull(plan, track1.view());
    CHECK(result == false);

    auto track2 = TestTrack{"Test", "Artist", "Album", "/path", 2019};
    result = evaluator.evaluateFull(plan, track2.view());
    CHECK(result == true);
  }

  TEST_CASE("PlanEvaluator combines duration and year comparisons", "[query][unit][plan_evaluator]")
  {
    // Note: genre comparison requires dictionary resolution, so we use numeric genreId
    auto expr = parseOk("@duration > 180000 && $year >= 2020");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);
    auto evaluator = PlanEvaluator{};

    auto track1 = TestTrack{"Test", "Artist", "Album", "/path", 2020, 5, 200000, 320000, 44100, 2, 16, 1, 2, 1};
    auto result = evaluator.evaluateFull(plan, track1.view());
    CHECK(result == true);

    auto track2 = TestTrack{"Test", "Artist", "Album", "/path", 2019, 5, 200000, 320000, 44100, 2, 16, 1, 2, 1};
    result = evaluator.evaluateFull(plan, track2.view());
    CHECK(result == false);
  }

  TEST_CASE("PlanEvaluator treats true plans as matching every track", "[query][unit][plan_evaluator]")
  {
    auto expr = parseOk("true");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);
    auto evaluator = PlanEvaluator{};

    CHECK(plan.matchesAll == true);

    auto track1 = TestTrack{};
    auto result = evaluator.matches(plan, track1.view());
    CHECK(result == true);

    auto track2 = TestTrack{"Test", "Artist", "Album", "/path", 1900};
    result = evaluator.matches(plan, track2.view());
    CHECK(result == true);
  }

  TEST_CASE("PlanEvaluator matches OR expressions combining LIKE and comparison operands",
            "[query][unit][plan_evaluator]")
  {
    // Test that $title ~ "Bach" or $year > 2021 evaluates correctly
    auto expr = parseOk(R"($title ~ "Bach" or $year > 2021)");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);
    auto evaluator = PlanEvaluator{};

    // Track with title containing "Bach"
    auto track1 = TestTrack{"Bach Greatest Hits", "Artist", "Album", "/path", 2021};
    auto result = evaluator.evaluateFull(plan, track1.view());
    CHECK(result == true); // matches title ~ "Bach"

    // Track with year > 2021 but title doesn't contain "Bach"
    auto track2 = TestTrack{"Classical Music", "Artist", "Album", "/path", 2022};
    result = evaluator.evaluateFull(plan, track2.view());
    CHECK(result == true); // matches year > 2021

    // Track with neither matching
    auto track3 = TestTrack{"Classical Music", "Artist", "Album", "/path", 2021};
    result = evaluator.evaluateFull(plan, track3.view());
    CHECK(result == false);
  }

  TEST_CASE("PlanEvaluator matches OR expressions combining numeric comparisons", "[query][unit][plan_evaluator]")
  {
    // Test $year > 2000 or $year > 1990 to verify OR works with two numeric comparisons
    auto expr = parseOk("$year > 2000 or $year > 1990");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);
    auto evaluator = PlanEvaluator{};

    // year 2021: 2021 > 2000 is true, so OR should be true
    auto track1 = TestTrack{"Title", "Artist", "Album", "/path", 2021};
    auto result = evaluator.evaluateFull(plan, track1.view());
    CHECK(result == true);

    // year 1995: 1995 > 2000 is false, but 1995 > 1990 is true, so OR should be true
    auto track2 = TestTrack{"Title", "Artist", "Album", "/path", 1995};
    result = evaluator.evaluateFull(plan, track2.view());
    CHECK(result == true);

    // year 1980: both are false, so OR should be false
    auto track3 = TestTrack{"Title", "Artist", "Album", "/path", 1980};
    result = evaluator.evaluateFull(plan, track3.view());
    CHECK(result == false);
  }
} // namespace ao::query::test
