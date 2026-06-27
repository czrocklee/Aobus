// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/query/PlanEvaluatorTestSupport.h"
#include <ao/query/PlanEvaluator.h>
#include <ao/query/QueryCompiler.h>

#include <catch2/catch_test_macros.hpp>

namespace ao::query::test
{
  TEST_CASE("PlanEvaluator matches unquoted title LIKE substrings", "[query][unit][plan_evaluator]")
  {
    auto expr = parseOk("$title ~ Test");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);
    auto evaluator = PlanEvaluator{};

    auto track1 = TestTrack{"Test Title"};
    auto result = evaluator.evaluateFull(plan, track1.view());
    CHECK(result == true);

    auto track2 = TestTrack{"Another Title"};
    result = evaluator.evaluateFull(plan, track2.view());
    CHECK(result == false);
  }

  TEST_CASE("PlanEvaluator matches title equality case-sensitively", "[query][unit][plan_evaluator]")
  {
    auto expr = parseOk("$title = 'Hello World'");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);
    auto evaluator = PlanEvaluator{};

    auto track1 = TestTrack{"Hello World"};
    auto result = evaluator.evaluateFull(plan, track1.view());
    CHECK(result == true);

    auto track2 = TestTrack{"hello world"}; // case-sensitive
    result = evaluator.evaluateFull(plan, track2.view());
    CHECK(result == false);

    auto track3 = TestTrack{"Hello"};
    result = evaluator.evaluateFull(plan, track3.view());
    CHECK(result == false);
  }

  TEST_CASE("PlanEvaluator matches title inequality case-sensitively", "[query][unit][plan_evaluator]")
  {
    auto expr = parseOk("$title != 'Hello'");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);
    auto evaluator = PlanEvaluator{};

    auto track1 = TestTrack{"Hello World"};
    auto result = evaluator.evaluateFull(plan, track1.view());
    CHECK(result == true);

    auto track2 = TestTrack{"Hello"};
    result = evaluator.evaluateFull(plan, track2.view());
    CHECK(result == false);
  }

  TEST_CASE("PlanEvaluator compares titles below a string bound", "[query][unit][plan_evaluator]")
  {
    auto expr = parseOk("$title < 'zoo'");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);
    auto evaluator = PlanEvaluator{};

    auto track1 = TestTrack{"apple"};
    auto result = evaluator.evaluateFull(plan, track1.view());
    CHECK(result == true);

    auto track2 = TestTrack{"zoo"};
    result = evaluator.evaluateFull(plan, track2.view());
    CHECK(result == false);

    auto track3 = TestTrack{"zooExtra"};
    result = evaluator.evaluateFull(plan, track3.view());
    CHECK(result == false);
  }

  TEST_CASE("PlanEvaluator compares titles above a string bound case-sensitively", "[query][unit][plan_evaluator]")
  {
    auto expr = parseOk("$title > 'apple'");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);
    auto evaluator = PlanEvaluator{};

    auto track1 = TestTrack{"banana"};
    auto result = evaluator.evaluateFull(plan, track1.view());
    CHECK(result == true);

    auto track2 = TestTrack{"apple"};
    result = evaluator.evaluateFull(plan, track2.view());
    CHECK(result == false);

    auto track3 = TestTrack{"Apple"}; // case-sensitive
    result = evaluator.evaluateFull(plan, track3.view());
    CHECK(result == false);
  }

  TEST_CASE("PlanEvaluator matches quoted title LIKE substrings", "[query][unit][plan_evaluator]")
  {
    // Simple title LIKE test with quoted string
    auto expr = parseOk(R"($title ~ "Bach")");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);
    auto evaluator = PlanEvaluator{};

    // Track with title containing "Bach"
    auto track1 = TestTrack{"Bach Greatest Hits"};
    auto result = evaluator.evaluateFull(plan, track1.view());
    CHECK(result == true);

    // Track with title not containing "Bach"
    auto track2 = TestTrack{"Mozart Symphony"};
    result = evaluator.evaluateFull(plan, track2.view());
    CHECK(result == false);

    // Track with exact match
    auto track3 = TestTrack{"Bach"};
    result = evaluator.evaluateFull(plan, track3.view());
    CHECK(result == true);
  }

  TEST_CASE("PlanEvaluator matches quoted title LIKE on multi-field tracks", "[query][unit][plan_evaluator]")
  {
    // Test LIKE with a Track that has multiple fields set
    auto expr = parseOk(R"($title ~ "Bach")");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);
    auto evaluator = PlanEvaluator{};

    auto track = TestTrack{"Bach Greatest Hits", "Artist", "Album", "/path", 2021};
    auto result = evaluator.evaluateFull(plan, track.view());
    CHECK(result == true);
  }
} // namespace ao::query::test
