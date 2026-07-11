// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/CoreIds.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/uimodel/library/list/ListActionPolicy.h>
#include <ao/uimodel/library/presentation/TrackPresentationRecommender.h>

#include <string_view>

namespace ao::uimodel
{
  ListActionViewState describeListActions(ListId selectedListId, bool selectedListHasChildren)
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

  ListId parentForNewSmartList(ListId selectedListId)
  {
    if (selectedListId != kInvalidListId && selectedListId != rt::kAllTracksListId)
    {
      return selectedListId;
    }

    return kInvalidListId;
  }

  bool canReorderListTracks(ListPresentationSourceKind sourceKind, std::string_view presentationId)
  {
    return sourceKind == ListPresentationSourceKind::Manual && presentationId == rt::kListOrderTrackPresentationId;
  }
} // namespace ao::uimodel
