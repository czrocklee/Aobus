// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/query/PlanEvaluatorTestSupport.h"
#include <ao/AudioCodec.h>
#include <ao/query/PlanEvaluator.h>
#include <ao/query/QueryCompiler.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>

namespace ao::query::test
{
  TEST_CASE("PlanEvaluator matches bitrate comparisons", "[query][unit][plan_evaluator]")
  {
    auto expr = parseOk("@bitrate >= 320000");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);
    auto evaluator = PlanEvaluator{};

    auto track1 = TestTrack{"Test", "Artist", "Album", "/path", 2020, 5, 180000, 320000};
    auto result = evaluator.evaluateFull(plan, track1.view());
    CHECK(result == true);

    auto track2 = TestTrack{"Test", "Artist", "Album", "/path", 2020, 5, 180000, 256000};
    result = evaluator.evaluateFull(plan, track2.view());
    CHECK(result == false);
  }

  TEST_CASE("PlanEvaluator matches sample-rate comparisons", "[query][unit][plan_evaluator]")
  {
    auto expr = parseOk("@sampleRate >= 48000");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);
    auto evaluator = PlanEvaluator{};

    auto track1 = TestTrack{"Test", "Artist", "Album", "/path", 2020, 5, 180000, 320000, 48000};
    auto result = evaluator.evaluateFull(plan, track1.view());
    CHECK(result == true);

    auto track2 = TestTrack{"Test", "Artist", "Album", "/path", 2020, 5, 180000, 320000, 44100};
    result = evaluator.evaluateFull(plan, track2.view());
    CHECK(result == false);
  }

  TEST_CASE("PlanEvaluator evaluates duration bitrate and sample-rate unit constants", "[query][unit][plan_evaluator]")
  {
    auto expr = parseOk("@duration >= 3m && @bitrate >= 256k && @sampleRate >= 44.1k");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);
    auto evaluator = PlanEvaluator{};

    auto track1 = TestTrack{"Test", "Artist", "Album", "/path", 2020, 5, 180000, 320000, 44100};
    auto result = evaluator.evaluateFull(plan, track1.view());
    CHECK(result == true);

    auto track2 = TestTrack{"Test", "Artist", "Album", "/path", 2020, 5, 180000, 192000, 44100};
    result = evaluator.evaluateFull(plan, track2.view());
    CHECK(result == false);

    auto track3 = TestTrack{"Test", "Artist", "Album", "/path", 2020, 5, 180000, 320000, 32000};
    result = evaluator.evaluateFull(plan, track3.view());
    CHECK(result == false);
  }

  TEST_CASE("PlanEvaluator scales duration and bitrate units before comparison", "[query][unit][plan_evaluator]")
  {
    auto const spec = TrackSpec{
      .duration = std::chrono::minutes{3} + std::chrono::seconds{5}, // 3m 5s
      .bitrate = 320000                                              // 320k
    };

    auto track = TrackFixture{spec};
    auto evaluator = PlanEvaluator{};
    auto compiler = QueryCompiler{};

    CHECK(evaluator.evaluateFull(compileOk(compiler, parseOk("@duration > 3m")), track.view()) == true);
    CHECK(evaluator.evaluateFull(compileOk(compiler, parseOk("@duration > 4m")), track.view()) == false);
    CHECK(evaluator.evaluateFull(compileOk(compiler, parseOk("@bitrate = 320k")), track.view()) == true);
  }

  TEST_CASE("PlanEvaluator matches AAC codec expressions", "[query][unit][plan_evaluator]")
  {
    auto spec = TrackSpec{};
    spec.codec = AudioCodec::Aac;
    auto track = TestTrack{spec};

    auto compiler = QueryCompiler{&track.dictionary()};
    auto evaluator = PlanEvaluator{};
    auto plan = compileOk(compiler, parseOk("@codec = AAC"));

    CHECK(evaluator.evaluateFull(plan, track.view()) == true);
  }
} // namespace ao::query::test
