// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <functional>

namespace ao::winui::layout
{
  /**
   * @brief Persistent Windows pane sizes a shell component reads and writes.
   *
   * Navigation and inspector boundaries belong to `DesktopSettings`,
   * which atomically owns them together with window, shell, and track-view
   * state. Components receive that authority through these accessors instead of
   * mapping it into the generic component-state store, which cannot express
   * participation in the shared settings candidate.
   */
  struct PaneSettingsAccess final
  {
    std::function<double()> navigationWidth;
    std::function<void(double)> setNavigationWidth;
    std::function<double()> inspectorWidth;
    std::function<void(double)> setInspectorWidth;
    /// Persist the current settings candidate after a completed drag.
    std::function<void()> commit;
  };
} // namespace ao::winui::layout
