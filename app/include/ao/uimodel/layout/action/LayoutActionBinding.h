// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/uimodel/layout/action/LayoutActionSlot.h>

namespace ao::uimodel
{
  struct LayoutActionBindingProperty final
  {
    LayoutActionSlot slot = LayoutActionSlot::PrimaryClick;
  };
} // namespace ao::uimodel
