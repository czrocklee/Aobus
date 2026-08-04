// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "layout/runtime/LayoutComponent.h"
#include <ao/Error.h>

#include <memory>

namespace ao::uimodel
{
  struct LayoutNode;
}

namespace ao::winui::layout
{
  struct LayoutBuildContext;

  /**
   * @brief Build the track table: the column header strip and the row list.
   *
   * Both are driven by the compiled item templates the window frame owns, so
   * the component constructs the controls and resolves those templates rather
   * than describing cells itself. It fails the candidate when a template it
   * needs is missing, because a shipped frame without them is a build defect.
   */
  Result<std::unique_ptr<LayoutComponent>> makeTrackTable(LayoutBuildContext& ctx, uimodel::LayoutNode const& node);
} // namespace ao::winui::layout
