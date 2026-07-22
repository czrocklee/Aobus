// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/CoreIds.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/uimodel/library/track/TrackPageRoute.h>

namespace ao::uimodel
{
  ListId smartListParentIdFromPage(ListId pageListId)
  {
    if (pageListId == rt::kAllTracksListId)
    {
      return kInvalidListId;
    }

    return pageListId;
  }
} // namespace ao::uimodel
