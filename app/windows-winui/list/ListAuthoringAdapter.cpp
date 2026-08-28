// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/winui/list/ListAuthoringAdapter.h>

#include <ao/CoreIds.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/uimodel/library/list/ListTreeProjection.h>
#include <ao/uimodel/library/presentation/ListPresentations.h>

#include <map>
#include <string_view>

namespace ao::winui
{
  bool listTreeChangeRequiresRebuild(rt::LibraryChangeSet const& changeSet) noexcept
  {
    return changeSet.libraryReset || !changeSet.listsUpserted.empty() || !changeSet.listsDeleted.empty();
  }

  rt::TrackPresentationSpec resolveListAuthoringPresentation(uimodel::ListPresentations const& listPresentations,
                                                             ListId const listId,
                                                             std::string_view const localExpression)
  {
    return listPresentations.presentationForList(uimodel::ListPresentationContext{
      .listId = listId,
      .sourceKind = uimodel::ListPresentationSourceKind::SavedList,
      .listExpression = localExpression,
    });
  }

  ListTreeRestoreState restoreListTreeState(uimodel::ListTreeProjection const& projection,
                                            ListId const activeListId,
                                            std::map<ListId, bool> const& previousExpansion)
  {
    auto state = ListTreeRestoreState{};
    state.selectedListId = projection.rowsById.contains(activeListId) ? activeListId : rt::kAllTracksListId;

    if (!projection.rowsById.contains(state.selectedListId))
    {
      state.selectedListId = projection.rootIds.empty() ? kInvalidListId : projection.rootIds.front();
    }

    for (auto const& [id, row] : projection.rowsById)
    {
      if (auto const previous = previousExpansion.find(id); previous != previousExpansion.end())
      {
        state.expandedById.emplace(id, previous->second);
      }
      else
      {
        state.expandedById.emplace(id, !row.childIds.empty());
      }
    }

    auto currentId = state.selectedListId;
    auto remaining = projection.rowsById.size();

    while (remaining-- > 0)
    {
      auto const current = projection.rowsById.find(currentId);

      if (current == projection.rowsById.end())
      {
        break;
      }

      auto const parentId = current->second.parentId;

      if (parentId == kInvalidListId || parentId == currentId)
      {
        break;
      }

      if (auto const parentExpansion = state.expandedById.find(parentId); parentExpansion != state.expandedById.end())
      {
        parentExpansion->second = true;
      }

      currentId = parentId;
    }

    return state;
  }
} // namespace ao::winui
