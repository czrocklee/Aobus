// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/uimodel/layout/action/LayoutActionSlot.h>

#include <string_view>

namespace ao::uimodel
{
  struct LayoutActionBindingProperty final
  {
    LayoutActionSlot slot = LayoutActionSlot::PrimaryClick;
  };

  struct LayoutActionBindingContext final
  {
    LayoutActionSlot slot = LayoutActionSlot::PrimaryClick;
    bool hasAnchor = false;
    bool hasFocusedView = false;
    std::string_view componentType = {};
  };
} // namespace ao::uimodel
