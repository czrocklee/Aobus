// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "TrackRegistry.h"

#include "TrackComponentRegistrations.h"
#include "layout/runtime/ComponentRegistry.h"

namespace ao::gtk::layout
{
  void registerTrackComponents(ComponentRegistry& registry)
  {
    registerTrackQuickFilterComponent(registry);
    registerTrackPresentationButtonComponent(registry);
  }

  void registerTrackDetailComponents(ComponentRegistry& registry)
  {
    registerTrackDetailScopeComponent(registry);
    registerTrackSelectionRegionComponent(registry);
    registerTrackCoverArtComponent(registry);
    registerTrackFieldGridComponent(registry);
    registerTrackEditLockComponent(registry);
  }

  void registerTrackEditorComponents(ComponentRegistry& registry)
  {
    registerTrackTagEditorComponent(registry);
  }
} // namespace ao::gtk::layout
