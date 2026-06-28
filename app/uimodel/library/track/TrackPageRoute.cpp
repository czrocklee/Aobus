// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/Type.h>
#include <ao/rt/CorePrimitives.h>
#include <ao/uimodel/library/track/TrackPageRoute.h>

#include <utility>
#include <vector>

namespace ao::uimodel
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
} // namespace ao::uimodel
