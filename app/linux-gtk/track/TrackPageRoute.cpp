// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackPageRoute.h"

#include "ao/Type.h"
#include <ao/rt/CorePrimitives.h>

#include <utility>
#include <vector>

namespace ao::gtk
{
  TrackPageSelectionRoute describeSelectionRoute(rt::ViewId viewId, std::vector<TrackId> selectedIds)
  {
    auto route = TrackPageSelectionRoute{};
    route.focusedViewId = viewId;
    route.selectedIds = std::move(selectedIds);
    route.shouldUpdateRuntimeSelection = (viewId != rt::kInvalidViewId);
    return route;
  }

  ListId smartListParentIdFromPage(ListId pageListId)
  {
    if (pageListId == rt::kAllTracksListId)
    {
      return kInvalidListId;
    }

    return pageListId;
  }
} // namespace ao::gtk
