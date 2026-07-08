// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "test/unit/query/ExecutionPlanTestSupport.h"
#include <ao/query/detail/Bytecode.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <tuple>

namespace ao::query::test
{
  TEST_CASE("ExecutionPlan - scales duration unit constants", "[query][unit][execution-plan]")
  {
    auto expr = parseOk("@duration >= 3m");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);

    auto it = std::ranges::find(plan.instructions, OpCode::LoadConstant, &Instruction::op);

    REQUIRE(it != plan.instructions.end());
    CHECK(it->constValue == 180000);
  }

  TEST_CASE("ExecutionPlan - scales bitrate unit constants", "[query][unit][execution-plan]")
  {
    auto expr = parseOk("@bitrate >= 2m");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);

    auto it = std::ranges::find(plan.instructions, OpCode::LoadConstant, &Instruction::op);

    REQUIRE(it != plan.instructions.end());
    CHECK(it->constValue == 2000000);
  }

  TEST_CASE("ExecutionPlan - scales sample-rate unit constants", "[query][unit][execution-plan]")
  {
    auto expr = parseOk("@sampleRate = 44.1k");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);

    auto it = std::ranges::find(plan.instructions, OpCode::LoadConstant, &Instruction::op);

    REQUIRE(it != plan.instructions.end());
    CHECK(it->constValue == 44100);
  }

  TEST_CASE("ExecutionPlan - rejects unit constants on unsupported fields", "[query][unit][execution-plan]")
  {
    auto expr = parseOk("$year >= 3m");
    auto compiler = QueryCompiler{};

    std::ignore = compileError(compiler, expr);
  }

  TEST_CASE("ExecutionPlan - scales unit literals", "[query][unit][execution-plan]")
  {
    auto compiler = QueryCompiler{};

    SECTION("Duration Supports MsSMMHUnits")
    {
      struct Case final
      {
        std::string unit;
        std::int64_t expected;
      };
      auto cases = {Case{.unit = "1ms", .expected = 1},
                    Case{.unit = "1s", .expected = 1000},
                    Case{.unit = "1m", .expected = 60000},
                    Case{.unit = "1h", .expected = 3600000}};

      for (auto const& c : cases)
      {
        auto expr = parseOk("@duration >= " + c.unit);
        auto plan = compileOk(compiler, expr);
        auto it = std::ranges::find(plan.instructions, OpCode::LoadConstant, &Instruction::op);
        CHECK(it->constValue == c.expected);
      }
    }

    SECTION("DurationSupportsCompoundUnits")
    {
      auto expr = parseOk("@duration >= 2m30s");
      CHECK(compileOk(compiler, expr).instructions[1].constValue == 150000);
    }

    SECTION("Bitrate and SampleRate Support KAndMUnits")
    {
      auto expr1 = parseOk("@bitrate >= 256k");
      CHECK(compileOk(compiler, expr1).instructions[1].constValue == 256000);

      auto expr2 = parseOk("@sampleRate >= 44.1k");
      CHECK(compileOk(compiler, expr2).instructions[1].constValue == 44100);
    }

    SECTION("Unit Suffix Is CaseInsensitive")
    {
      auto expr = parseOk("@bitrate >= 256K");
      CHECK(compileOk(compiler, expr).instructions[1].constValue == 256000);
    }

    SECTION("Negative Unit Literal Compiles")
    {
      auto expr = parseOk("@bitrate >= -2k");
      CHECK(compileOk(compiler, expr).instructions[1].constValue == -2000);
    }
  }

  TEST_CASE("ExecutionPlan - rejects invalid unit literals", "[query][unit][execution-plan]")
  {
    auto compiler = QueryCompiler{};

    SECTION("Rejects UnsupportedSuffixForField")
    {
      std::ignore = compileError(compiler, parseOk("@duration >= 10k"));
      std::ignore = compileError(compiler, parseOk("@bitrate >= 3h"));
      std::ignore = compileError(compiler, parseOk("@sampleRate >= 44h"));
      std::ignore = compileError(compiler, parseOk("@channels = 2h"));
      std::ignore = compileError(compiler, parseOk("@bitDepth = 16h"));
      std::ignore = compileError(compiler, parseOk("$year = 2020h"));
      std::ignore = compileError(compiler, parseOk("$trackNumber = 1h"));
      std::ignore = compileError(compiler, parseOk("$trackTotal = 10h"));
      std::ignore = compileError(compiler, parseOk("$discNumber = 1h"));
      std::ignore = compileError(compiler, parseOk("$discTotal = 2h"));
      std::ignore = compileError(compiler, parseOk("%custom = 1h"));
    }

    SECTION("Rejects OutOfRangeIntegerParsing")
    {
      std::ignore = compileError(compiler, parseOk("@bitrate >= 9999999999999999999999k"));

      // checkedMul overflow (value * 1000 overflows)
      std::ignore = compileError(compiler, parseOk("@duration >= 1844674407370955161s"));

      // checkedAdd overflow (value * 10 + fraction overflows)
      std::ignore = compileError(compiler, parseOk("@duration >= 1844674407370955161.6ms"));
    }

    SECTION("Rejects NonIntegerResolution")
    {
      std::ignore = compileError(compiler, parseOk("@duration >= 1.5ms"));
    }

    SECTION("RejectsCompoundUnitsOutsideDuration")
    {
      std::ignore = compileError(compiler, parseOk("@bitrate >= 2k3m"));
    }

    SECTION("Accepts Zero")
    {
      auto plan = compileOk(compiler, parseOk("@duration >= 0s"));
      CHECK(plan.instructions[1].constValue == 0);
    }

    SECTION("Rejects MissingNumericFieldContext")
    {
      // A top-level unit constant expression like "3m" should fail
      std::ignore = compileError(compiler, parseOk("3m"));
    }
  }
} // namespace ao::query::test
