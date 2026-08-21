// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "test/unit/query/ExecutionPlanTestSupport.h"
#include <ao/query/detail/Bytecode.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <tuple>
#include <vector>

namespace ao::query::test
{
  TEST_CASE("ExecutionPlan - compiles text substring operators as Unicode caseless",
            "[query][unit][execution-plan][unicode]")
  {
    auto expr = parseOk("$title ~ Love");
    auto plan = compileOk(expr);

    CHECK(std::ranges::any_of(
      plan.instructions, [](Instruction const& instruction) { return instruction.op == OpCode::Like; }));
  }

  TEST_CASE("ExecutionPlan - compiles Unicode caseless substring keys", "[query][unit][execution-plan][unicode]")
  {
    auto const plan = compileOk(parseOk("$title ~ 'STRASSE Cafe\u0301'"));

    REQUIRE(plan.stringConstants.size() == 1);
    CHECK(plan.stringConstants[0] == "strasse café");
    CHECK(std::ranges::any_of(
      plan.instructions, [](Instruction const& instruction) { return instruction.op == OpCode::Like; }));
  }

  TEST_CASE("ExecutionPlan - validates substring operands", "[query][unit][execution-plan][unicode]")
  {
    CHECK(compileError(parseOk("$title ~ 123")).code == Error::Code::FormatRejected);
    CHECK(compileError(parseOk("@duration ~ 'three minutes'")).code == Error::Code::FormatRejected);
    CHECK(compileError(parseOk("$coverArt ~ 'front'")).code == Error::Code::FormatRejected);
    CHECK(compileError(parseOk("#rock ~ 'progressive'")).code == Error::Code::FormatRejected);
  }

  TEST_CASE("ExecutionPlan - compiles string constants", "[query][unit][execution-plan][string]")
  {
    auto expr = parseOk("$title = 'Hello World'");
    auto plan = compileOk(expr);

    CHECK_FALSE(plan.stringConstants.empty());
    CHECK(plan.stringConstants[0] == "Hello World");
  }

  TEST_CASE("ExecutionPlan - compiles LIKE for album ids", "[query][unit][execution-plan][string]")
  {
    auto expr = parseOk(R"($album ~ "Greatest Hits")");
    auto plan = compileOk(expr);

    CHECK_FALSE(plan.instructions.empty());
  }

  TEST_CASE("ExecutionPlan - compiles LIKE for genre ids", "[query][unit][execution-plan][string]")
  {
    auto expr = parseOk(R"($genre ~ "Rock")");
    auto plan = compileOk(expr);

    CHECK_FALSE(plan.instructions.empty());
  }

  TEST_CASE("ExecutionPlan - compiles LIKE for album artist ids", "[query][unit][execution-plan][string]")
  {
    auto expr = parseOk(R"($albumArtist ~ "Bach")");
    auto plan = compileOk(expr);

    CHECK_FALSE(plan.instructions.empty());
  }

  TEST_CASE("ExecutionPlan - rejects LIKE for cover art ids", "[query][unit][execution-plan][string]")
  {
    auto expr = parseOk(R"($coverArt ~ "front")");
    std::ignore = compileError(expr);
  }

  TEST_CASE("ExecutionPlan - rejects LIKE for tags", "[query][unit][execution-plan][string]")
  {
    auto expr = parseOk(R"(#rock ~ "progressive")");
    std::ignore = compileError(expr);
  }

  TEST_CASE("ExecutionPlan - compiles LIKE for titles", "[query][unit][execution-plan][string]")
  {
    auto expr = parseOk(R"($title ~ "Bach")");
    auto plan = compileOk(expr);

    CHECK_FALSE(plan.instructions.empty());
    CHECK_FALSE(plan.matchesAll);
  }

  TEST_CASE("ExecutionPlan - compiles mixed LIKE and EQUAL in OR expressions", "[query][unit][execution-plan][string]")
  {
    // This tests that leftField is correctly saved before compiling right operand
    // $title ~ "Bach" should NOT check if ArtistId is used with LIKE
    auto expr = parseOk(R"($title ~ "Bach" or $artist = "Bach")");
    auto plan = compileOk(expr);

    CHECK_FALSE(plan.instructions.empty());
    CHECK_FALSE(plan.matchesAll);
  }

  TEST_CASE("ExecutionPlan - compiles parenthesized LIKE and EQUAL in OR expressions",
            "[query][unit][execution-plan][string]")
  {
    // Explicit grouping with parentheses should also work
    auto expr = parseOk(R"(($title ~ "Bach") or ($artist = "Bach"))");
    auto plan = compileOk(expr);

    CHECK_FALSE(plan.instructions.empty());
    CHECK_FALSE(plan.matchesAll);
  }

  TEST_CASE("ExecutionPlan - compiles multiple OR branches with ID field equality",
            "[query][unit][execution-plan][string]")
  {
    // Multiple ID field equalities in OR should compile without throwing
    auto expr = parseOk(R"($artist = "Bach" or $artist = "Mozart" or $album = "交响乐")");
    auto plan = compileOk(expr);

    CHECK_FALSE(plan.instructions.empty());
    CHECK_FALSE(plan.matchesAll);
  }

  TEST_CASE("ExecutionPlan - compiles title LIKE chained with AND", "[query][unit][execution-plan][string]")
  {
    // Title LIKE should work with AND
    auto expr = parseOk(R"($title ~ "Bach" and $year > 2000)");
    auto plan = compileOk(expr);

    CHECK_FALSE(plan.instructions.empty());
    CHECK_FALSE(plan.matchesAll);
  }

  TEST_CASE("ExecutionPlan - deduplicates string constants", "[query][unit][execution-plan][string]")
  {
    SECTION("Reuses Identical String Constants")
    {
      auto expr = parseOk(R"($title = "Bach" or $title != "Bach")");
      auto plan = compileOk(expr);
      CHECK(plan.stringConstants.size() == 1);
      CHECK(plan.stringConstants[0] == "Bach");
    }

    SECTION("Stores Different String Constants Separately")
    {
      auto expr = parseOk(R"($title = "Bach" or $title = "Mozart")");
      auto plan = compileOk(expr);
      CHECK(plan.stringConstants.size() == 2);
    }
  }
} // namespace ao::query::test
