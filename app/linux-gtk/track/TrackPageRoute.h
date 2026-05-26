// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "ao/Type.h"
#include <ao/rt/CorePrimitives.h>

#include <vector>

namespace ao::gtk
{
  struct TrackPageSelectionRoute
  {
    rt::ViewId focusedViewId = rt::kInvalidViewId;
    std::vector<TrackId> selectedIds;
    bool shouldUpdateRuntimeSelection = false;
  };

  TrackPageSelectionRoute describeSelectionRoute(rt::ViewId viewId, std::vector<TrackId> selectedIds);

  ListId smartListParentIdFromPage(ListId pageListId);
} // namespace ao::gtk
