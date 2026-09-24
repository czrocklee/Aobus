// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/query/Expression.h>
#include <ao/query/Field.h>
#include <ao/query/detail/Bytecode.h>

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace ao::query::test
{
  Expression parseOk(std::string_view text);
  ExecutionPlan compileOk(Expression const& expr);
  Error compileError(Expression const& expr);

  using InstructionIterator = std::vector<Instruction>::const_iterator;

  // Returns the zero-based occurrence of op; a missing occurrence fails the case.
  InstructionIterator findInstruction(ExecutionPlan const& plan, OpCode op, std::size_t occurrence = 0);

  // Checks the LoadField, LoadConstant, comparison triple that ends at the
  // given occurrence of op and returns the comparison.
  InstructionIterator checkComparison(ExecutionPlan const& plan,
                                      OpCode op,
                                      Field field,
                                      std::int64_t constantValue,
                                      std::uint32_t dictionarySymbol = kNoDictionarySymbol,
                                      std::size_t occurrence = 0);
} // namespace ao::query::test
