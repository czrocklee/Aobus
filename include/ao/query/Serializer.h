// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <ao/query/Expression.h>

namespace ao::query
{
  std::string serialize(Expression const& expr);
}
