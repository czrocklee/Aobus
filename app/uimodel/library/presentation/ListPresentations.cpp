// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/library/presentation/ListPresentations.h>

#include <ao/CoreIds.h>
#include <ao/rt/Log.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/uimodel/library/presentation/TrackPresentationCatalog.h>

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::uimodel
{
  ListPresentations::ListPresentations(TrackPresentationCatalog& catalog)
    : _catalog{catalog}
  {
  }

  ListPresentations::ListPresentations(TrackPresentationCatalog& catalog, rt::LibraryChanges const& changes)
    : _catalog{catalog}
  {
    _changesSubscription = changes.onChanged(
      [this](rt::LibraryChangeSet const& changeSet)
      {
        auto removedListIds = std::vector<ListId>{};
        removedListIds.reserve(changeSet.listsDeleted.size());

        for (auto const listId : changeSet.listsDeleted)
        {
          if (_presentations.erase(listId) > 0)
          {
            removedListIds.push_back(listId);
          }
        }

        // No owner state is touched after notification starts: an observer may
        // synchronously destroy this owner. The shared signal state stays alive
        // long enough to deliver the snapshotted ids.
        auto const changedPtr = _changedPtr;

        for (auto const listId : removedListIds)
        {
          changedPtr->emit(listId);
        }
      });
  }

  ListPresentations::~ListPresentations() = default;

  void ListPresentations::restore(Snapshot presentations)
  {
    if (_presentations == presentations)
    {
      return;
    }

    _presentations = std::move(presentations);
    _changedPtr->emit(kInvalidListId);
  }

  std::optional<std::string_view> ListPresentations::presentationIdForList(ListId const listId) const
  {
    if (auto const it = _presentations.find(listId); it != _presentations.end())
    {
      return it->second;
    }

    return std::nullopt;
  }

  void ListPresentations::setPresentationIdForList(ListId const listId, std::string_view const presentationId)
  {
    if (listId == kInvalidListId)
    {
      return;
    }

    auto const strId = std::string{presentationId};

    if (strId.empty())
    {
      clearPresentationForList(listId);
      return;
    }

    if (auto const it = _presentations.find(listId); it != _presentations.end() && it->second == strId)
    {
      return;
    }

    _presentations[listId] = strId;
    _changedPtr->emit(listId);
  }

  void ListPresentations::clearPresentationForList(ListId const listId)
  {
    if (_presentations.erase(listId) > 0)
    {
      _changedPtr->emit(listId);
    }
  }

  rt::TrackPresentationSpec ListPresentations::presentationForList(ListPresentationContext const& context) const
  {
    if (auto const optId = presentationIdForList(context.listId); optId)
    {
      if (auto const optSpec = _catalog.specForId(*optId); optSpec)
      {
        return *optSpec;
      }

      APP_LOG_DEBUG(
        "ListPresentations: saved presentation id '{}' is unavailable; using recommendation fallback", *optId);
    }

    return recommendListPresentation(context, _catalog.builtinPresets(), _catalog.customPresentations());
  }
} // namespace ao::uimodel
