// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/library/list/ListActions.h>

#include <ao/CoreIds.h>
#include <ao/rt/VirtualListIds.h>

namespace ao::uimodel
{
  ListActionViewState describeListActions(ListId selectedListId, bool selectedListHasChildren)
  {
    auto state = ListActionViewState{};

    state.canCreate = true;

    if (!rt::isVirtualListId(selectedListId))
    {
      state.canEdit = true;
      state.canDelete = !selectedListHasChildren;
      state.canDeleteSubtree = selectedListHasChildren;
    }

    return state;
  }

  ListId parentForNewSmartList(ListId selectedListId)
  {
    if (!rt::isVirtualListId(selectedListId))
    {
      return selectedListId;
    }

    return kInvalidListId;
  }
} // namespace ao::uimodel
