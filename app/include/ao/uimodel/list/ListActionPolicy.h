// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "ao/Type.h"

namespace ao::uimodel::list
{
  struct ListActionViewState final
  {
    bool canCreate = false;
    bool canEdit = false;
    bool canDelete = false;
  };

  class ListActionPolicy final
  {
  public:
    static ListActionViewState describeActions(ListId selectedListId, bool selectedListHasChildren);

    static ListId parentForNewSmartList(ListId selectedListId);
  };
} // namespace ao::uimodel::list
