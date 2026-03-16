// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace rs::tag
{
  using Blob = std::vector<char>;
  using ValueType = std::variant<std::monostate, std::string, std::int64_t, double, Blob>;

  inline bool isNull(ValueType const& value) { return value == ValueType{}; }
}
