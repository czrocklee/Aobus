// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>

namespace ao::uimodel
{
  struct ListActionViewState final
  {
    bool canCreate = false;
    bool canEdit = false;
    bool canDelete = false;
  };

  ListActionViewState describeListActions(ListId selectedListId, bool selectedListHasChildren);
  ListId parentForNewSmartList(ListId selectedListId);
} // namespace ao::uimodel
