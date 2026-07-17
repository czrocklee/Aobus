// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/ThemePreset.h"

#include <ao/uimodel/preference/ThemePreset.h>

#include <string_view>

namespace ao::gtk
{
  std::string_view themeCssClass(uimodel::ThemePreset preset)
  {
    switch (preset)
    {
      case uimodel::ThemePreset::Classic: return "ao-theme-classic";
      case uimodel::ThemePreset::Modern: return "ao-theme-modern";
      default: return "ao-theme-classic";
    }
  }
} // namespace ao::gtk
