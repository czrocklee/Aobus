// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

namespace ao::uimodel
{
  struct LayoutComponentStateDocument;
  struct LayoutDocument;

  /**
   * @brief Promote runtime panel sizes from @p stateDoc into @p doc defaults.
   *
   * Matching `split.positionPercent` becomes `initialPositionPercent`, and
   * `collapsibleSplit.size` becomes `position`. Promoted entries are removed
   * from @p stateDoc; non-promoted values (e.g. `revealed`) are kept with a
   * refreshed baseline hash.
   *
   * @return Whether any panel size was promoted.
   */
  bool promotePanelSizeDefaults(LayoutDocument& doc, LayoutComponentStateDocument& stateDoc);
} // namespace ao::uimodel
