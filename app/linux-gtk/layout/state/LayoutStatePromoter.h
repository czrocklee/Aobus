// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "layout/document/LayoutDocument.h"

#include <cstddef>

namespace ao::gtk::layout
{
  struct LayoutComponentStateDocument;
  struct PanelSizePromotionResult final
  {
    bool changed = false;
    std::size_t promotedCount = 0;
    std::size_t residualCount = 0;
  };

  /**
   * @brief Promote runtime panel sizes from @p stateDoc into @p doc defaults.
   *
   * Matching `split.positionPercent` becomes `initialPositionPercent`, and
   * `collapsibleSplit.size` becomes `position`. Promoted entries are removed
   * from @p stateDoc; non-promoted values (e.g. `revealed`) are kept with a
   * refreshed baseline hash.
   */
  PanelSizePromotionResult promotePanelSizeDefaults(LayoutDocument& doc, LayoutComponentStateDocument& stateDoc);
} // namespace ao::gtk::layout
