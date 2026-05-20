// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "ao/query/Expression.h"

#include <string_view>

namespace ao::query
{
  Expression parse(std::string_view expr);
}
