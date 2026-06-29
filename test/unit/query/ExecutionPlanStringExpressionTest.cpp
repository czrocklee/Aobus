// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "test/unit/query/ExecutionPlanTestUtils.h"
#include <ao/query/detail/Bytecode.h>

#include <catch2/catch_test_macros.hpp>

#include <tuple>

namespace ao::query::test
{
  TEST_CASE("ExecutionPlan - compiles like operators", "[query][unit][execution_plan][string]")
  {
    auto expr = parseOk("$title ~ Love");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);

    bool hasLike = false;

    for (auto const& instr : plan.instructions)
    {
      if (instr.op == OpCode::Like)
      {
        hasLike = true;
        break;
      }
    }

    CHECK(hasLike == true);
  }

  TEST_CASE("ExecutionPlan - compiles string constants", "[query][unit][execution_plan][string]")
  {
    auto expr = parseOk("$title = 'Hello World'");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);

    CHECK_FALSE(plan.stringConstants.empty());
    CHECK(plan.stringConstants[0] == "Hello World");
  }

  TEST_CASE("ExecutionPlan - compiles LIKE for album ids", "[query][unit][execution_plan][string]")
  {
    auto expr = parseOk(R"($album ~ "Greatest Hits")");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);

    CHECK_FALSE(plan.instructions.empty());
  }

  TEST_CASE("ExecutionPlan - compiles LIKE for genre ids", "[query][unit][execution_plan][string]")
  {
    auto expr = parseOk(R"($genre ~ "Rock")");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);

    CHECK_FALSE(plan.instructions.empty());
  }

  TEST_CASE("ExecutionPlan - compiles LIKE for album artist ids", "[query][unit][execution_plan][string]")
  {
    auto expr = parseOk(R"($albumArtist ~ "Bach")");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);

    CHECK_FALSE(plan.instructions.empty());
  }

  TEST_CASE("ExecutionPlan - rejects LIKE for cover art ids", "[query][unit][execution_plan][string]")
  {
    auto expr = parseOk(R"($coverArt ~ "front")");
    auto compiler = QueryCompiler{};
    std::ignore = compileError(compiler, expr);
  }

  TEST_CASE("ExecutionPlan - rejects LIKE for tags", "[query][unit][execution_plan][string]")
  {
    auto expr = parseOk(R"(#rock ~ "progressive")");
    auto compiler = QueryCompiler{};
    std::ignore = compileError(compiler, expr);
  }

  TEST_CASE("ExecutionPlan - compiles LIKE for titles", "[query][unit][execution_plan][string]")
  {
    auto expr = parseOk(R"($title ~ "Bach")");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);

    CHECK_FALSE(plan.instructions.empty());
    CHECK_FALSE(plan.matchesAll);
  }

  TEST_CASE("ExecutionPlan - compiles mixed LIKE and EQUAL in OR expressions", "[query][unit][execution_plan][string]")
  {
    // This tests that leftField is correctly saved before compiling right operand
    // $title ~ "Bach" should NOT check if ArtistId is used with LIKE
    auto expr = parseOk(R"($title ~ "Bach" or $artist = "Bach")");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);

    CHECK_FALSE(plan.instructions.empty());
    CHECK_FALSE(plan.matchesAll);
  }

  TEST_CASE("ExecutionPlan - compiles parenthesized LIKE and EQUAL in OR expressions",
            "[query][unit][execution_plan][string]")
  {
    // Explicit grouping with parentheses should also work
    auto expr = parseOk(R"(($title ~ "Bach") or ($artist = "Bach"))");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);

    CHECK_FALSE(plan.instructions.empty());
    CHECK_FALSE(plan.matchesAll);
  }

  TEST_CASE("ExecutionPlan - compiles multiple OR branches with ID field equality",
            "[query][unit][execution_plan][string]")
  {
    // Multiple ID field equalities in OR should compile without throwing
    auto expr = parseOk(R"($artist = "Bach" or $artist = "Mozart" or $album = "交响乐")");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);

    CHECK_FALSE(plan.instructions.empty());
    CHECK_FALSE(plan.matchesAll);
  }

  TEST_CASE("ExecutionPlan - compiles title LIKE chained with AND", "[query][unit][execution_plan][string]")
  {
    // Title LIKE should work with AND
    auto expr = parseOk(R"($title ~ "Bach" and $year > 2000)");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);

    CHECK_FALSE(plan.instructions.empty());
    CHECK_FALSE(plan.matchesAll);
  }

  TEST_CASE("ExecutionPlan - deduplicates string constants", "[query][unit][execution_plan][string]")
  {
    SECTION("Reuses Identical String Constants")
    {
      auto expr = parseOk(R"($title = "Bach" or $title != "Bach")");
      auto compiler = QueryCompiler{};
      auto plan = compileOk(compiler, expr);
      CHECK(plan.stringConstants.size() == 1);
      CHECK(plan.stringConstants[0] == "Bach");
    }

    SECTION("Stores Different String Constants Separately")
    {
      auto expr = parseOk(R"($title = "Bach" or $title = "Mozart")");
      auto compiler = QueryCompiler{};
      auto plan = compileOk(compiler, expr);
      CHECK(plan.stringConstants.size() == 2);
    }
  }
} // namespace ao::query::test
