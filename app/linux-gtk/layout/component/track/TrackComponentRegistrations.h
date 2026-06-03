// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "layout/runtime/ComponentRegistry.h"

namespace ao::gtk::layout
{
  void registerTrackQuickFilterComponent(ComponentRegistry& registry);
  void registerTrackPresentationButtonComponent(ComponentRegistry& registry);

  void registerTrackDetailScopeComponent(ComponentRegistry& registry);
  void registerTrackSelectionRegionComponent(ComponentRegistry& registry);
  void registerTrackCoverArtComponent(ComponentRegistry& registry);
  void registerTrackFieldGridComponent(ComponentRegistry& registry);

  void registerTrackTagEditorComponent(ComponentRegistry& registry);
} // namespace ao::gtk::layout
