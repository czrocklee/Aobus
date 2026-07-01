// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/ThemePreset.h"

#include <ao/rt/AppPrefsState.h>

#include <string_view>

namespace ao::gtk
{
  std::string_view themeCssClass(rt::ThemePresetId preset)
  {
    switch (preset)
    {
      case rt::ThemePresetId::Classic: return "ao-theme-classic";
      case rt::ThemePresetId::Modern: return "ao-theme-modern";
      default: return "ao-theme-classic";
    }
  }
} // namespace ao::gtk
