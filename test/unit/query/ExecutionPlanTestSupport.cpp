// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "ExecutionPlanTestSupport.h"

#include <ao/Error.h>
#include <ao/query/ExecutionPlan.h>
#include <ao/query/Expression.h>
#include <ao/query/Field.h>
#include <ao/query/Parser.h>
#include <ao/query/QueryCompilation.h>
#include <ao/query/detail/Bytecode.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>

namespace ao::query::test
{
  Expression parseOk(std::string_view const text)
  {
    auto res = ::ao::query::parse(text);
    REQUIRE(res.has_value());
    return std::move(*res);
  }

  ExecutionPlan compileOk(Expression const& expr)
  {
    auto res = compileQuery(expr);
    REQUIRE(res.has_value());
    return std::move(*res);
  }

  Error compileError(Expression const& expr)
  {
    auto res = compileQuery(expr);
    REQUIRE_FALSE(res.has_value());
    CHECK(res.error().code == Error::Code::FormatRejected);
    return res.error();
  }

  InstructionIterator findInstruction(ExecutionPlan const& plan, OpCode op, std::size_t occurrence)
  {
    auto instruction = plan.instructions.begin();

    for (std::size_t i = 0; i <= occurrence; ++i)
    {
      instruction = std::ranges::find(instruction, plan.instructions.end(), op, &Instruction::op);
      REQUIRE(instruction != plan.instructions.end());

      if (i < occurrence)
      {
        ++instruction;
      }
    }

    return instruction;
  }

  InstructionIterator checkComparison(ExecutionPlan const& plan,
                                      OpCode op,
                                      Field field,
                                      std::int64_t constantValue,
                                      std::uint32_t dictionarySymbol,
                                      std::size_t occurrence)
  {
    auto const comparison = findInstruction(plan, op, occurrence);
    REQUIRE(comparison - plan.instructions.begin() >= 2);
    auto const& load = *(comparison - 2);
    auto const& constant = *(comparison - 1);
    REQUIRE(load.op == OpCode::LoadField);
    REQUIRE(constant.op == OpCode::LoadConstant);
    CHECK(load.field == static_cast<std::uint8_t>(field));
    CHECK(comparison->field == static_cast<std::uint8_t>(field));
    CHECK(constant.constValue == constantValue);
    CHECK(constant.operand == load.operand + 1);
    CHECK(comparison->operand == constant.operand);
    CHECK(comparison->dictionarySymbol == dictionarySymbol);
    return comparison;
  }
} // namespace ao::query::test
