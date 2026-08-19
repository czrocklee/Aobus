// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/query/ExecutionPlanTestSupport.h"
#include "test/unit/query/PlanEvaluatorTestSupport.h"
#include <ao/query/PlanEvaluator.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string_view>

namespace ao::query::test
{
  TEST_CASE("PlanEvaluator - matches unquoted title LIKE substrings", "[query][unit][plan-evaluator]")
  {
    auto expr = parseOk("$title ~ Test");
    auto plan = compileOk(expr);
    auto evaluator = PlanEvaluator{};

    auto track1 = TestTrack{"Test Title"};
    auto result = evaluator.evaluateFull(plan, track1.view());
    CHECK(result == true);

    auto track2 = TestTrack{"Another Title"};
    result = evaluator.evaluateFull(plan, track2.view());
    CHECK(result == false);
  }

  TEST_CASE("PlanEvaluator - matches Unicode caseless title substrings", "[query][unit][plan-evaluator][unicode]")
  {
    auto const plan = compileOk(parseOk(R"($title ~ "DVOŘÁK STRASSE")"));
    auto evaluator = PlanEvaluator{};
    auto track = TestTrack{"Dvořák Straße"};

    CHECK(evaluator.evaluateFull(plan, track.view()));
  }

  TEST_CASE("PlanEvaluator - applies full Unicode case folding", "[query][unit][plan-evaluator][unicode]")
  {
    auto evaluator = PlanEvaluator{};
    auto germanTrack = TestTrack{"Die Straße"};
    auto greekTrack = TestTrack{"Οδυσσεύς"};

    CHECK(evaluator.evaluateFull(compileOk(parseOk(R"($title ~ "STRASSE")")), germanTrack.view()));
    CHECK(evaluator.evaluateFull(compileOk(parseOk(R"($title ~ "ΟΔΥΣΣΕΎΣ")")), greekTrack.view()));
  }

  TEST_CASE("PlanEvaluator - keeps URI substring matching byte-exact", "[query][unit][plan-evaluator][unicode]")
  {
    auto evaluator = PlanEvaluator{};
    auto track = TestTrack{TrackSpec{.uri = "Music/Cafe\u0301.flac"}};
    auto const matches = [&](std::string_view const text)
    {
      auto plan = ExecutionPlan{};
      plan.stringConstants.emplace_back(text);
      plan.instructions.push_back(
        {.op = OpCode::LoadField, .field = static_cast<std::uint8_t>(Field::Uri), .operand = 0});
      plan.instructions.push_back({.op = OpCode::LoadConstant, .operand = 1, .constValue = 0});
      plan.instructions.push_back({.op = OpCode::Like, .field = static_cast<std::uint8_t>(Field::Uri), .operand = 1});
      return evaluator.evaluateFull(plan, track.view());
    };

    CHECK(matches("Cafe\u0301"));
    CHECK_FALSE(matches("Café"));
    CHECK_FALSE(matches("music/"));
  }

  TEST_CASE("PlanEvaluator - matches title equality case-sensitively", "[query][unit][plan-evaluator]")
  {
    auto expr = parseOk("$title = 'Hello World'");
    auto plan = compileOk(expr);
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

  TEST_CASE("PlanEvaluator - matches title inequality case-sensitively", "[query][unit][plan-evaluator]")
  {
    auto expr = parseOk("$title != 'Hello'");
    auto plan = compileOk(expr);
    auto evaluator = PlanEvaluator{};

    auto track1 = TestTrack{"Hello World"};
    auto result = evaluator.evaluateFull(plan, track1.view());
    CHECK(result == true);

    auto track2 = TestTrack{"Hello"};
    result = evaluator.evaluateFull(plan, track2.view());
    CHECK(result == false);
  }

  TEST_CASE("PlanEvaluator - compares titles below a string bound", "[query][unit][plan-evaluator]")
  {
    auto expr = parseOk("$title < 'zoo'");
    auto plan = compileOk(expr);
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

  TEST_CASE("PlanEvaluator - compares titles above a string bound case-sensitively", "[query][unit][plan-evaluator]")
  {
    auto expr = parseOk("$title > 'apple'");
    auto plan = compileOk(expr);
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

  TEST_CASE("PlanEvaluator - matches quoted title LIKE substrings", "[query][unit][plan-evaluator]")
  {
    // Simple title LIKE test with quoted string
    auto expr = parseOk(R"($title ~ "Bach")");
    auto plan = compileOk(expr);
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

  TEST_CASE("PlanEvaluator - matches quoted title LIKE on multi-field tracks", "[query][unit][plan-evaluator]")
  {
    // Test LIKE with a Track that has multiple fields set
    auto expr = parseOk(R"($title ~ "Bach")");
    auto plan = compileOk(expr);
    auto evaluator = PlanEvaluator{};

    auto track = TestTrack{"Bach Greatest Hits", "Artist", "Album", "path", 2021};
    auto result = evaluator.evaluateFull(plan, track.view());
    CHECK(result == true);
  }
} // namespace ao::query::test
