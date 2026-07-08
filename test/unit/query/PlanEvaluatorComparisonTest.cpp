// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/query/PlanEvaluatorTestSupport.h"
#include <ao/query/PlanEvaluator.h>
#include <ao/query/QueryCompiler.h>

#include <catch2/catch_test_macros.hpp>

namespace ao::query::test
{
  TEST_CASE("PlanEvaluator - matches year equality and rejects different years", "[query][unit][plan-evaluator]")
  {
    auto expr = parseOk("$year = 2020");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);
    auto evaluator = PlanEvaluator{};

    auto track1 = TestTrack{"Test", "Artist", "Album", "/path", 2020};
    auto result = evaluator.evaluateFull(plan, track1.view());
    CHECK(result == true);

    auto track2 = TestTrack{"Test", "Artist", "Album", "/path", 2019};
    result = evaluator.evaluateFull(plan, track2.view());
    CHECK(result == false);
  }

  TEST_CASE("PlanEvaluator - matches duration above an exclusive threshold", "[query][unit][plan-evaluator]")
  {
    auto expr = parseOk("@duration > 179000");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);
    auto evaluator = PlanEvaluator{};

    auto track1 = TestTrack{"Test", "Artist", "Album", "/path", 2020, 5, 180000};
    auto result = evaluator.evaluateFull(plan, track1.view());
    CHECK(result == true);

    auto track2 = TestTrack{"Test", "Artist", "Album", "/path", 2020, 5, 170000};
    result = evaluator.evaluateFull(plan, track2.view());
    CHECK(result == false);
  }

  TEST_CASE("PlanEvaluator - matches year inequality only for different years", "[query][unit][plan-evaluator]")
  {
    auto expr = parseOk("$year != 2020");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);
    auto evaluator = PlanEvaluator{};

    auto track1 = TestTrack{"Test", "Artist", "Album", "/path", 2020, 5, 180000};
    auto result = evaluator.evaluateFull(plan, track1.view());
    CHECK(result == false);

    auto track2 = TestTrack{"Test", "Artist", "Album", "/path", 2021, 5, 180000};
    result = evaluator.evaluateFull(plan, track2.view());
    CHECK(result == true);

    auto track3 = TestTrack{"Test", "Artist", "Album", "/path", 2019, 5, 180000};
    result = evaluator.evaluateFull(plan, track3.view());
    CHECK(result == true);
  }

  TEST_CASE("PlanEvaluator - matches duration at an inclusive threshold", "[query][unit][plan-evaluator]")
  {
    auto expr = parseOk("@duration >= 180000");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);
    auto evaluator = PlanEvaluator{};

    auto track1 = TestTrack{"Test", "Artist", "Album", "/path", 2020, 5, 180000};
    auto result = evaluator.evaluateFull(plan, track1.view());
    CHECK(result == true);

    auto track2 = TestTrack{"Test", "Artist", "Album", "/path", 2020, 5, 179999};
    result = evaluator.evaluateFull(plan, track2.view());
    CHECK(result == false);
  }

  TEST_CASE("PlanEvaluator - matches years below an exclusive threshold", "[query][unit][plan-evaluator]")
  {
    auto expr = parseOk("$year < 2021");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);
    auto evaluator = PlanEvaluator{};

    auto track1 = TestTrack{"Test", "Artist", "Album", "/path", 2020};
    auto result = evaluator.evaluateFull(plan, track1.view());
    CHECK(result == true);

    auto track2 = TestTrack{"Test", "Artist", "Album", "/path", 2021};
    result = evaluator.evaluateFull(plan, track2.view());
    CHECK(result == false);
  }

  TEST_CASE("PlanEvaluator - matches year equality on multi-field tracks", "[query][unit][plan-evaluator]")
  {
    // Note: $trackNumber is a cold field, so we use $year instead which is hot
    auto expr = parseOk("$year = 2020");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);
    auto evaluator = PlanEvaluator{};

    auto track1 = TestTrack{"Test", "Artist", "Album", "/path", 2020, 5};
    auto result = evaluator.evaluateFull(plan, track1.view());
    CHECK(result == true);

    auto track2 = TestTrack{"Test", "Artist", "Album", "/path", 2019, 5};
    result = evaluator.evaluateFull(plan, track2.view());
    CHECK(result == false);
  }

  TEST_CASE("PlanEvaluator - rejects year greater-than below threshold", "[query][unit][plan-evaluator]")
  {
    // Verify $year > 2000 returns false for year 1980
    auto expr = parseOk("$year > 2000");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);
    auto evaluator = PlanEvaluator{};

    auto track = TestTrack{"Title", "Artist", "Album", "/path", 1980};
    auto result = evaluator.evaluateFull(plan, track.view());
    CHECK(result == false);
  }

  TEST_CASE("PlanEvaluator - matches year greater-than only above threshold", "[query][unit][plan-evaluator]")
  {
    // Simple year > test to verify year comparison works
    auto expr = parseOk("$year > 2000");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);
    auto evaluator = PlanEvaluator{};

    auto track1 = TestTrack{"Title", "Artist", "Album", "/path", 2021};
    auto result = evaluator.evaluateFull(plan, track1.view());
    CHECK(result == true);

    auto track2 = TestTrack{"Title", "Artist", "Album", "/path", 1990};
    result = evaluator.evaluateFull(plan, track2.view());
    CHECK(result == false);
  }
} // namespace ao::query::test
