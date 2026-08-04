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
   * @brief Build the cover art of whatever the focused view has selected.
   *
   * The component owns its artwork end to end: it reads the shared detail
   * projection, hands the resource to its own presenter, and draws the authored
   * placeholder style when the selection carries no artwork. Its size is not
   * its own, though - the frame gives it a width, and it keeps itself square
   * inside whatever it is given.
   */
  Result<std::unique_ptr<LayoutComponent>> makeTrackCoverArt(LayoutBuildContext& ctx, uimodel::LayoutNode const& node);
} // namespace ao::winui::layout
