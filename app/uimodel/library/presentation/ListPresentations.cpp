// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/library/presentation/ListPresentations.h>

#include "RetainKnownLists.h"
#include <ao/CoreIds.h>
#include <ao/rt/ListNode.h>
#include <ao/rt/Log.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibrarySnapshot.h>
#include <ao/uimodel/library/presentation/TrackPresentationCatalog.h>

#include <optional>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

namespace ao::uimodel
{
  bool isTrackPresentationEligibleForList(ListId const listId, rt::TrackPresetId const& id) noexcept
  {
    return listId != rt::kAllTracksListId || id != rt::TrackPresetId{rt::BuiltinPreset::ListOrder};
  }

  ListPresentations::ListPresentations(TrackPresentationCatalog& catalog, rt::LibraryChanges const& changes)
    : _catalog{catalog}
  {
    _changesSubscription = changes.onChanged(
      [this](rt::LibraryChangeSet const& changeSet)
      {
        auto removedListIds = std::vector<ListId>{};

        if (changeSet.libraryReset)
        {
          removedListIds.reserve(_presentations.size());

          for (auto const listId : _presentations | std::views::keys)
          {
            removedListIds.push_back(listId);
          }

          _presentations.clear();
        }
        else
        {
          removedListIds.reserve(changeSet.listsDeleted.size());

          for (auto const listId : changeSet.listsDeleted)
          {
            if (_presentations.erase(listId) > 0)
            {
              removedListIds.push_back(listId);
            }
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

  void ListPresentations::restore(Snapshot presentations, std::span<ListId const> const knownListIds)
  {
    retainKnownLists(presentations, knownListIds);

    if (_presentations == presentations)
    {
      return;
    }

    _presentations = std::move(presentations);
    _changedPtr->emit(kInvalidListId);
  }

  std::optional<rt::TrackPresetId> ListPresentations::presentationIdForList(ListId const listId) const
  {
    if (auto const it = _presentations.find(listId); it != _presentations.end())
    {
      return it->second;
    }

    return std::nullopt;
  }

  void ListPresentations::setPresentationIdForList(ListId const listId, rt::TrackPresetId const presentationId)
  {
    if (listId == kInvalidListId)
    {
      return;
    }

    if (auto const it = _presentations.find(listId); it != _presentations.end() && it->second == presentationId)
    {
      return;
    }

    _presentations.insert_or_assign(listId, presentationId);
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
    if (auto const optId = presentationIdForList(context.listId);
        optId && isTrackPresentationEligibleForList(context.listId, *optId))
    {
      if (auto const optSpec = _catalog.specForId(*optId); optSpec)
      {
        return *optSpec;
      }

      APP_LOG_DEBUG("ListPresentations: saved presentation id '{}' is unavailable; using recommendation fallback",
                    rt::trackPresetKey(*optId));
    }

    return recommendListPresentation(context, _catalog.builtinPresets());
  }

  rt::TrackPresentationSpec ListPresentations::presentationForList(ListId const listId,
                                                                   rt::Library const& library) const
  {
    if (!rt::isVirtualListId(listId))
    {
      if (auto const optNode = library.snapshot().listNode(listId); optNode)
      {
        return presentationForList({
          .listId = listId,
          .sourceKind = ListPresentationSourceKind::SavedList,
          .listExpression = optNode->expression,
        });
      }
    }

    return presentationForList({.listId = listId});
  }
} // namespace ao::uimodel
