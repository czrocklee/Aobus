// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/CoreIds.h>
#include <ao/library/ListStore.h>
#include <ao/library/ListView.h>
#include <ao/library/MusicLibrary.h>
#include <ao/rt/StorageResult.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/source/ManualListSource.h>
#include <ao/rt/source/SmartListSource.h>
#include <ao/rt/source/TrackSource.h>
#include <ao/rt/source/TrackSourceCache.h>

#include <algorithm>
#include <memory>
#include <ranges>
#include <utility>
#include <vector>

namespace ao::rt
{
  namespace
  {
    TrackSource* parentSourceOf(TrackSource& source)
    {
      if (auto* const manual = dynamic_cast<ManualListSource*>(&source); manual != nullptr)
      {
        return manual->source();
      }

      if (auto* const smart = dynamic_cast<SmartListSource*>(&source); smart != nullptr)
      {
        return &smart->source();
      }

      return nullptr;
    }
  } // namespace

  TrackSourceCache::TrackSourceCache(library::MusicLibrary& library, LibraryChanges const& changes)
    : _library{library}, _allTracks{_library.tracks()}, _smartEvaluator{_library}
  {
    _listsMutatedSubscription = changes.onListsMutated(
      [this](LibraryChanges::ListsMutated const& ev)
      {
        for (auto const id : ev.deleted)
        {
          eraseList(id);
        }

        for (auto const id : ev.upserted)
        {
          refreshList(id);
        }
      });

    _trackCollectionChangedSubscription = changes.onTrackCollectionChanged(
      [this](LibraryChanges::TrackCollectionChanged const& ev)
      {
        for (auto const id : ev.inserted)
        {
          _allTracks.notifyInserted(id);
        }

        for (auto const id : ev.deleted)
        {
          _allTracks.notifyRemoved(id);
        }
      });
  }

  TrackSourceCache::~TrackSourceCache() = default;

  TrackSource& TrackSourceCache::allTracks()
  {
    return _allTracks;
  }

  TrackSource& TrackSourceCache::sourceFor(ListId const listId)
  {
    return getOrBuildSource(listId);
  }

  void TrackSourceCache::reloadAllTracks()
  {
    auto const transaction = _library.readTransaction();
    _allTracks.reloadFromStore(transaction);
  }

  void TrackSourceCache::refreshList(ListId const listId)
  {
    if (listId == kInvalidListId)
    {
      return;
    }

    auto const it = _sources.find(listId);

    if (it == _sources.end())
    {
      return;
    }

    auto const transaction = _library.readTransaction();
    auto const optView =
      storageValueOrNullopt(_library.lists().reader(transaction).get(listId), "Failed to refresh list source");

    if (!optView)
    {
      eraseList(listId);
      return;
    }

    if (auto* const manual = dynamic_cast<ManualListSource*>(it->second.get()); manual != nullptr)
    {
      manual->reloadFromListView(*optView);
    }
    else if (auto* const smart = dynamic_cast<SmartListSource*>(it->second.get()); smart != nullptr)
    {
      smart->setExpression(std::string{optView->filter()});
      smart->reload();
    }
  }

  void TrackSourceCache::eraseList(ListId const listId)
  {
    if (listId == kInvalidListId)
    {
      return;
    }

    // A correct implementation needs to destroy children before parents.
    // For now, since we only erase by ID, we simply erase it.
    // If we need child-before-parent, we should search through `_sources`
    // for any sources whose `_source` pointer points to this list's source.
    // But since it's an unordered (flat) map, we'd have to walk the graph.
    // Given that `WorkspaceService` handles closing views,
    // this might be sufficient for now.

    // Find all children recursively that depend on this list and erase them too
    auto toErase = std::vector<ListId>{};
    toErase.push_back(listId);

    bool foundNew = true;

    while (foundNew)
    {
      foundNew = false;

      for (auto const& [childId, childSource] : _sources)
      {
        if (std::ranges::contains(toErase, childId))
        {
          continue;
        }

        auto* const parent = parentSourceOf(*childSource);

        for (auto const id : toErase)
        {
          if (auto const it = _sources.find(id); it != _sources.end())
          {
            if (parent == it->second.get())
            {
              toErase.push_back(childId);
              foundNew = true;
              break;
            }
          }
        }

        if (foundNew)
        {
          break; // restart outer loop to avoid iterator invalidation issues mentally
        }
      }
    }

    // Erase in reverse order of discovery (children first)
    for (auto& it : std::ranges::reverse_view{toErase})
    {
      _sources.erase(it);
    }
  }

  TrackSource& TrackSourceCache::getOrBuildSource(ListId const listId)
  {
    if (listId == kInvalidListId)
    {
      return _allTracks;
    }

    if (auto const it = _sources.find(listId); it != _sources.end())
    {
      return *it->second;
    }

    auto const transaction = _library.readTransaction();
    auto const optView =
      storageValueOrNullopt(_library.lists().reader(transaction).get(listId), "Failed to build list source");

    if (!optView)
    {
      // Fallback to all tracks if missing
      return _allTracks;
    }

    auto& parentSource = getOrBuildSource(optView->parentId());

    if (optView->isSmart())
    {
      auto smartSourcePtr = std::make_unique<SmartListSource>(parentSource, _library, _smartEvaluator);
      smartSourcePtr->setExpression(std::string{optView->filter()});
      smartSourcePtr->reload();
      auto& ref = *smartSourcePtr;
      _sources.emplace(listId, std::move(smartSourcePtr));
      return ref;
    }

    auto manualSourcePtr = std::make_unique<ManualListSource>(*optView, &parentSource);
    auto& ref = *manualSourcePtr;
    _sources.emplace(listId, std::move(manualSourcePtr));
    return ref;
  }
} // namespace ao::rt
