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
   * @brief Build the track detail region: the metadata and audio-property sections.
   *
   * The component owns the section chrome and the adapter that drives it. Which
   * fields exist, whether a section is expanded, and how a value reads stay
   * decisions of `TrackDetailControl` against the shared schema; owning it here
   * is what makes those elements live and die with the generation that built
   * them. The artwork belongs to `track.coverArt`, not to this region.
   */
  Result<std::unique_ptr<LayoutComponent>> makeTrackDetail(LayoutBuildContext& ctx, uimodel::LayoutNode const& node);
} // namespace ao::winui::layout
