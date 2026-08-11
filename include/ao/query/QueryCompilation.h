// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/query/Expression.h>

// This value-returning compilation boundary requires the complete plan type.
// Storage-only consumers can include <ao/query/ExecutionPlan.h> instead.
#include <ao/query/detail/Bytecode.h>

namespace ao::query
{
  /**
   * Compile an expression AST into an execution plan (non-throwing entry point).
   *
   * @param expr The expression AST to compile.
   * @return The compiled ExecutionPlan, or an Error{Code::FormatRejected, ...} if @p expr is not
   *         a valid query predicate. Never throws on invalid input.
   */
  Result<ExecutionPlan> compileQuery(Expression const& expr);
} // namespace ao::query
