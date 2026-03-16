// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <rs/expr/Expression.h>

namespace rs::expr
{
  Expression parse(std::string_view const expr);
}
