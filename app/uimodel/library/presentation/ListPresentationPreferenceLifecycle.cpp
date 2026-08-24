// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/uimodel/library/presentation/ListPresentationPreferenceLifecycle.h>

#include <ao/CoreIds.h>
#include <ao/compat/MoveOnlyFunction.h>
#include <ao/rt/library/LibraryChanges.h>

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace ao::uimodel
{
  ListPresentationPreferenceLifecycle::ListPresentationPreferenceLifecycle(
    std::map<ListId, std::string>& presentations,
    rt::LibraryChanges const& changes,
    compat::MoveOnlyFunction<void(ListId)> onPreferenceRemoved)
  {
    _changesSubscription = changes.onChanged(
      [presentationMap = &presentations,
       onPreferenceRemoved = std::move(onPreferenceRemoved)](rt::LibraryChangeSet const& changeSet) mutable
      {
        auto removedListIds = std::vector<ListId>{};
        removedListIds.reserve(changeSet.listsDeleted.size());

        for (auto const listId : changeSet.listsDeleted)
        {
          if (presentationMap->erase(listId) > 0)
          {
            removedListIds.push_back(listId);
          }
        }

        // Finish every access to the externally owned map before invoking an
        // observer. The first callback may synchronously destroy both the
        // lifecycle and the map; Signal pins this active handler until emit
        // returns, so the snapshotted ids remain safe to deliver.
        if (onPreferenceRemoved)
        {
          for (auto const listId : removedListIds)
          {
            onPreferenceRemoved(listId);
          }
        }
      });
  }
} // namespace ao::uimodel
