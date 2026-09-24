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
  namespace
  {
    std::int64_t requireLoadConstant(ExecutionPlan const& plan)
    {
      auto const it = std::ranges::find(plan.instructions, OpCode::LoadConstant, &Instruction::op);
      REQUIRE(it != plan.instructions.end());
      return it->constValue;
    }
  } // namespace

  TEST_CASE("ExecutionPlan - scales duration unit constants", "[query][unit][execution-plan]")
  {
    auto expr = parseOk("@duration >= 3m");
    auto plan = compileOk(expr);

    CHECK(requireLoadConstant(plan) == 180000);
  }

  TEST_CASE("ExecutionPlan - scales bitrate unit constants", "[query][unit][execution-plan]")
  {
    auto expr = parseOk("@bitrate >= 2m");
    auto plan = compileOk(expr);

    CHECK(requireLoadConstant(plan) == 2000000);
  }

  TEST_CASE("ExecutionPlan - scales sample-rate unit constants", "[query][unit][execution-plan]")
  {
    auto expr = parseOk("@sampleRate = 44.1k");
    auto plan = compileOk(expr);

    CHECK(requireLoadConstant(plan) == 44100);
  }

  TEST_CASE("ExecutionPlan - rejects unit constants on unsupported fields", "[query][unit][execution-plan]")
  {
    auto expr = parseOk("$year >= 3m");

    std::ignore = compileError(expr);
  }

  TEST_CASE("ExecutionPlan - scales unit literals", "[query][unit][execution-plan]")
  {
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
        auto plan = compileOk(expr);
        CHECK(requireLoadConstant(plan) == c.expected);
      }
    }

    SECTION("DurationSupportsCompoundUnits")
    {
      auto expr = parseOk("@duration >= 2m30s");
      auto plan = compileOk(expr);
      CHECK(requireLoadConstant(plan) == 150000);
    }

    SECTION("Bitrate and SampleRate Support KAndMUnits")
    {
      auto expr1 = parseOk("@bitrate >= 256k");
      auto plan1 = compileOk(expr1);
      CHECK(requireLoadConstant(plan1) == 256000);

      auto expr2 = parseOk("@sampleRate >= 44.1k");
      auto plan2 = compileOk(expr2);
      CHECK(requireLoadConstant(plan2) == 44100);
    }

    SECTION("Unit Suffix Is CaseInsensitive")
    {
      auto expr = parseOk("@bitrate >= 256K");
      auto plan = compileOk(expr);
      CHECK(requireLoadConstant(plan) == 256000);
    }

    SECTION("Negative Unit Literal Compiles")
    {
      auto expr = parseOk("@bitrate >= -2k");
      auto plan = compileOk(expr);
      CHECK(requireLoadConstant(plan) == -2000);
    }

    SECTION("Accepts Zero")
    {
      auto plan = compileOk(parseOk("@duration >= 0s"));
      CHECK(requireLoadConstant(plan) == 0);
    }
  }

  TEST_CASE("ExecutionPlan - rejects invalid unit literals", "[query][unit][execution-plan]")
  {
    SECTION("Rejects UnsupportedSuffixForField")
    {
      std::ignore = compileError(parseOk("@duration >= 10k"));
      std::ignore = compileError(parseOk("@bitrate >= 3h"));
      std::ignore = compileError(parseOk("@sampleRate >= 44h"));
      std::ignore = compileError(parseOk("@channels = 2h"));
      std::ignore = compileError(parseOk("@bitDepth = 16h"));
      std::ignore = compileError(parseOk("$year = 2020h"));
      std::ignore = compileError(parseOk("$trackNumber = 1h"));
      std::ignore = compileError(parseOk("$trackTotal = 10h"));
      std::ignore = compileError(parseOk("$discNumber = 1h"));
      std::ignore = compileError(parseOk("$discTotal = 2h"));
      std::ignore = compileError(parseOk("%custom = 1h"));
    }

    SECTION("Rejects OutOfRangeIntegerParsing")
    {
      std::ignore = compileError(parseOk("@bitrate >= 9999999999999999999999k"));

      // checkedMul overflow (value * 1000 overflows)
      std::ignore = compileError(parseOk("@duration >= 1844674407370955161s"));

      // Combining the whole value and fraction overflows.
      std::ignore = compileError(parseOk("@duration >= 1844674407370955161.6ms"));
    }

    SECTION("Rejects NonIntegerResolution")
    {
      std::ignore = compileError(parseOk("@duration >= 1.5ms"));
    }

    SECTION("RejectsCompoundUnitsOutsideDuration")
    {
      std::ignore = compileError(parseOk("@bitrate >= 2k3m"));
    }

    SECTION("Rejects MissingNumericFieldContext")
    {
      // A top-level unit constant expression like "3m" should fail
      std::ignore = compileError(parseOk("3m"));
    }
  }
} // namespace ao::query::test
