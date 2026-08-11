// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/query/Expression.h>
#include <ao/query/detail/Bytecode.h>

#include <string_view>

namespace ao::query::test
{
  Expression parseOk(std::string_view text);
  ExecutionPlan compileOk(Expression const& expr);
  Error compileError(Expression const& expr);
} // namespace ao::query::test
