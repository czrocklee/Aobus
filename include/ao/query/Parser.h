// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/query/Expression.h>

namespace ao::query
{
  Expression parse(std::string_view const expr);
}
