// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <string_view>

namespace ao::uimodel::layout
{
  inline constexpr std::string_view kSplitComponentType = "split";
  inline constexpr std::string_view kCollapsibleSplitComponentType = "collapsibleSplit";

  constexpr bool isStatefulLayoutComponentType(std::string_view type) noexcept
  {
    return type == kSplitComponentType || type == kCollapsibleSplitComponentType;
  }
} // namespace ao::uimodel::layout
