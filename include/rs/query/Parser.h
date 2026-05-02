// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <rs/query/Expression.h>

namespace rs::query
{
  Expression parse(std::string_view const expr);
}
