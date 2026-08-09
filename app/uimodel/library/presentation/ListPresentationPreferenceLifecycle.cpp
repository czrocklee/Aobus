// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/uimodel/library/presentation/ListPresentationPreferenceLifecycle.h>

#include <ao/CoreIds.h>
#include <ao/rt/library/LibraryChanges.h>

#include <functional>
#include <map>
#include <string>
#include <utility>

namespace ao::uimodel
{
  ListPresentationPreferenceLifecycle::ListPresentationPreferenceLifecycle(
    std::map<ListId, std::string>& presentations,
    rt::LibraryChanges const& changes,
    std::move_only_function<void(ListId)> onPreferenceRemoved)
    : _presentations{presentations}, _onPreferenceRemoved{std::move(onPreferenceRemoved)}
  {
    _changesSubscription = changes.onChanged(
      [this](rt::LibraryChangeSet const& changeSet)
      {
        for (auto const listId : changeSet.listsDeleted)
        {
          if (_presentations.erase(listId) > 0 && _onPreferenceRemoved)
          {
            _onPreferenceRemoved(listId);
          }
        }
      });
  }
} // namespace ao::uimodel
