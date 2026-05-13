// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <sigc++/sigc++.h>

namespace ao::gtk
{
  /**
   * @brief A global signal bus for theme-related updates.
   *
   * Widgets and pages should connect to this signal to reload their
   * CSS providers and re-calculate any theme-dependent visual state.
   */
  sigc::signal<void()>& signalThemeRefresh();

  /**
   * @brief Emits the theme refresh signal with a small debounce.
   */
  void emitThemeRefresh();
}
