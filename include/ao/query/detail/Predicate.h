// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/query/Expression.h>

namespace ao::query::detail
{
  bool isPredicateExpression(Expression const& expr);
} // namespace ao::query::detail
