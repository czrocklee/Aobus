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
   * @brief Build the library navigation pane the authored `presentation` names.
   *
   * The two presentations are different Windows controls with the same job, so
   * each is its own component rather than one control that branches: a
   * `NavigationView` owns the workspace it wraps, while a `TreeView` occupies a
   * column beside it. Both own their persisted width and the boundary that
   * changes it.
   */
  Result<std::unique_ptr<LayoutComponent>> makeNavigationPane(LayoutBuildContext& ctx, uimodel::LayoutNode const& node);
} // namespace ao::winui::layout
