// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "ListSourceStore.h"

#include "LibraryMutationService.h"
#include "ManualListSource.h"
#include "SmartListSource.h"

#include <ao/library/ListStore.h>

#include <ao/library/ListView.h>

namespace ao::rt
{
  ListSourceStore::ListSourceStore(ao::library::MusicLibrary& library, LibraryMutationService& mutation)
    : _library{library}, _allTracks{_library.tracks()}, _smartEvaluator{_library}
  {
    _listsMutatedSubscription = mutation.onListsMutated(
      [this](LibraryMutationService::ListsMutated const& ev)
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
  }

  ListSourceStore::~ListSourceStore() = default;

  ao::rt::TrackSource& ListSourceStore::allTracks()
  {
    return _allTracks;
  }

  ao::rt::TrackSource& ListSourceStore::sourceFor(ao::ListId const listId)
  {
    return getOrBuildSource(listId);
  }

  void ListSourceStore::reloadAllTracks()
  {
    auto const txn = _library.readTransaction();
    _allTracks.reloadFromStore(txn);
  }

  void ListSourceStore::refreshList(ao::ListId const listId)
  {
    if (listId == ao::ListId{})
    {
      return;
    }

    auto const it = _sources.find(listId);

    if (it == _sources.end())
    {
      return;
    }

    auto const txn = _library.readTransaction();
    auto const optView = _library.lists().reader(txn).get(listId);

    if (!optView)
    {
      eraseList(listId);
      return;
    }

    if (auto* const manual = dynamic_cast<ManualListSource*>(it->second.get()))
    {
      manual->reloadFromListView(*optView);
    }
    else if (auto* const smart = dynamic_cast<SmartListSource*>(it->second.get()))
    {
      smart->setExpression(std::string{optView->filter()});
      smart->reload();
    }
  }

  void ListSourceStore::eraseList(ao::ListId const listId)
  {
    if (listId == ao::ListId{})
    {
      return;
    }

    // A correct implementation needs to destroy children before parents.
    // For now, since we only erase by ID, we simply erase it.
    // If we need child-before-parent, we should search through `_sources`
    // for any sources whose `_source` pointer points to this list's source.
    // But since it's an unordered_map, we'd have to walk the graph.
    // Given that `WorkspaceService` handles closing views,
    // this might be sufficient for now.

    // Find all children recursively that depend on this list and erase them too
    auto toErase = std::vector<ao::ListId>{};
    toErase.push_back(listId);

    bool foundNew = true;

    while (foundNew)
    {
      foundNew = false;

      for (auto const& [childId, childSource] : _sources)
      {
        if (std::ranges::find(toErase, childId) != toErase.end())
        {
          continue;
        }

        TrackSource* parentPtr = nullptr;

        if (auto* const manual = dynamic_cast<ManualListSource*>(childSource.get()))
        {
          parentPtr = manual->_source;
        }
        else if (auto* const smart = dynamic_cast<SmartListSource*>(childSource.get()))
        {
          parentPtr = &smart->source();
        }

        for (auto const id : toErase)
        {
          if (auto const it = _sources.find(id); it != _sources.end())
          {
            if (parentPtr == it->second.get())
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
    for (auto it = toErase.rbegin(); it != toErase.rend(); ++it)
    {
      _sources.erase(*it);
    }
  }

  TrackSource& ListSourceStore::getOrBuildSource(ao::ListId const listId)
  {
    if (listId == ao::ListId{})
    {
      return _allTracks;
    }

    if (auto const it = _sources.find(listId); it != _sources.end())
    {
      return *it->second;
    }

    auto const txn = _library.readTransaction();
    auto const optView = _library.lists().reader(txn).get(listId);

    if (!optView)
    {
      // Fallback to all tracks if missing
      return _allTracks;
    }

    auto& parentSource = getOrBuildSource(optView->parentId());

    if (optView->isSmart())
    {
      auto smartSource = std::make_unique<SmartListSource>(parentSource, _library, _smartEvaluator);
      smartSource->setExpression(std::string{optView->filter()});
      smartSource->reload();
      auto& ref = *smartSource;
      _sources.emplace(listId, std::move(smartSource));
      return ref;
    }

    auto manualSource = std::make_unique<ManualListSource>(*optView, &parentSource);
    auto& ref = *manualSource;
    _sources.emplace(listId, std::move(manualSource));
    return ref;
  }
}
