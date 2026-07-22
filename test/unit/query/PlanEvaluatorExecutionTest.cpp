// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/query/PlanEvaluatorTestSupport.h"
#include <ao/library/TrackView.h>
#include <ao/query/Field.h>
#include <ao/query/PlanEvaluator.h>
#include <ao/query/QueryCompiler.h>
#include <ao/query/detail/Bytecode.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <span>

namespace ao::query::test
{
  TEST_CASE("PlanEvaluator - rejects invalid string constant operands", "[query][unit][plan-evaluator]")
  {
    auto plan = ExecutionPlan{};
    plan.instructions.push_back(
      {.op = OpCode::LoadField, .field = static_cast<std::uint8_t>(Field::Title), .operand = 0});
    plan.instructions.push_back({.op = OpCode::LoadConstant, .operand = 1, .constValue = 999});
    plan.instructions.push_back({.op = OpCode::Eq, .operand = 1}); // out of bounds string index
    plan.instructions.push_back({.op = OpCode::LoadConstant, .operand = 1, .constValue = -1LL});
    plan.instructions.push_back({.op = OpCode::Eq, .operand = 1}); // < 0

    auto evaluator = PlanEvaluator{};
    auto track = TestTrack{"Title"};
    auto result = evaluator.evaluateFull(plan, track.view());
    CHECK(result == false);
  }

  TEST_CASE("PlanEvaluator - evaluates no-track-data plans without storage tiers", "[query][unit][plan-evaluator]")
  {
    auto expr = parseOk("true");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);
    auto evaluator = PlanEvaluator{};

    auto emptyView = library::TrackView{std::span<std::byte const>{}, std::span<std::byte const>{}};
    CHECK(plan.accessProfile == AccessProfile::NoTrackData);
    CHECK(evaluator.evaluateFull(plan, emptyView));
  }

  TEST_CASE("PlanEvaluator - executes cold-only plans with cold-only track views", "[query][unit][plan-evaluator]")
  {
    auto expr = parseOk("@duration >= 180000");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);
    auto evaluator = PlanEvaluator{};

    CHECK(plan.accessProfile == AccessProfile::ColdOnly);

    auto track = TestTrack{"Test", "Artist", "Album", "/path", 2020, 5, 180000};
    CHECK(evaluator.evaluateFull(plan, track.coldOnlyView()) == true);
  }

  TEST_CASE("PlanEvaluator - executes mixed-access plans with both storage tiers", "[query][unit][plan-evaluator]")
  {
    auto expr = parseOk("$year = 2020 && @duration >= 180000");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);
    auto evaluator = PlanEvaluator{};

    CHECK(plan.accessProfile == AccessProfile::HotAndCold);

    auto track = TestTrack{"Test", "Artist", "Album", "/path", 2020, 5, 180000};
    CHECK(evaluator.evaluateFull(plan, track.view()));
  }
} // namespace ao::query::test
