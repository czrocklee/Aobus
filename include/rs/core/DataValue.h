// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <string>
#include <variant>

namespace rs::core
{
  using DataValue = std::variant<std::monostate, bool, std::int64_t, std::string_view, std::string>;
}
