// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/uimodel/layout/component/SharedLayoutComponentType.h>

#include <string_view>

namespace ao::uimodel
{
  inline constexpr std::string_view kSplitComponentType = componentTypeName(SharedLayoutComponentType::Split);

  /// A GTK-only container so far, so it names itself rather than the vocabulary.
  inline constexpr std::string_view kCollapsibleSplitComponentType = "collapsibleSplit";

  constexpr bool isStatefulLayoutComponentType(std::string_view type) noexcept
  {
    return type == kSplitComponentType || type == kCollapsibleSplitComponentType;
  }
} // namespace ao::uimodel
