// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "ao/Type.h"
#include <ao/rt/CorePrimitives.h>
#include <ao/uimodel/list/ListActionPolicy.h>

namespace ao::uimodel::list
{
  ListActionViewState ListActionPolicy::describeActions(ListId selectedListId, bool selectedListHasChildren)
  {
    auto state = ListActionViewState{};

    state.canCreate = true;

    if (selectedListId != kInvalidListId && selectedListId != rt::kAllTracksListId)
    {
      state.canEdit = true;
      state.canDelete = !selectedListHasChildren;
    }

    return state;
  }

  ListId ListActionPolicy::parentForNewSmartList(ListId selectedListId)
  {
    if (selectedListId != kInvalidListId && selectedListId != rt::kAllTracksListId)
    {
      return selectedListId;
    }

    return kInvalidListId;
  }
} // namespace ao::uimodel::list
