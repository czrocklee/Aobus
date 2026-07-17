// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/uimodel/preference/ThemePreset.h>

#include <string_view>

namespace ao::gtk
{
  // Returns the GTK CSS class name (e.g., "ao-theme-modern") for the given preset.
  std::string_view themeCssClass(uimodel::ThemePreset preset);
} // namespace ao::gtk
