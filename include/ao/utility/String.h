// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <string>
#include <string_view>

namespace ao::utility
{
  std::string toLower(std::string_view text);
  std::string_view trim(std::string_view text);
} // namespace ao::utility
