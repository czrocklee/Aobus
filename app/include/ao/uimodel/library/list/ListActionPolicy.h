// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/uimodel/library/presentation/TrackPresentationRecommender.h>

#include <string_view>

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
  bool canReorderListTracks(ListPresentationSourceKind sourceKind, std::string_view presentationId);
} // namespace ao::uimodel
