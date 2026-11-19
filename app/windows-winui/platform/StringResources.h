// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <string>
#include <string_view>

namespace ao::winui
{
  std::string resourceString(std::string_view resourceId);
  std::string resourceStringOr(std::string_view resourceId, std::string_view fallback);
  std::string stableResourceString(std::string_view prefix, std::string_view stableId, std::string_view fallback);
} // namespace ao::winui
