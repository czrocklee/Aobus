// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/rt/AppPrefsState.h>

#include <string_view>

namespace ao::gtk
{
  std::string_view themePresetToString(rt::ThemePresetId preset);
  rt::ThemePresetId themePresetFromString(std::string_view str);

  // Returns the GTK CSS class name (e.g., "ao-theme-modern") for the given preset.
  std::string_view themeCssClass(rt::ThemePresetId preset);
} // namespace ao::gtk
