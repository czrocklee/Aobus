// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/source/TrackSourceCache.h>

#include "runtime/source/CachedListSource.h"
#include <ao/Contract.h>
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/compat/MoveOnlyFunction.h>
#include <ao/library/ListStore.h>
#include <ao/library/ListView.h>
#include <ao/library/MusicLibrary.h>
#include <ao/rt/TrackEditScript.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/source/AllTracksSource.h>
#include <ao/rt/source/ListOrderSource.h>
#include <ao/rt/source/SmartListSource.h>
#include <ao/rt/source/TrackSource.h>
#include <ao/rt/source/TrackSourceLease.h>
#include <ao/utility/StrongTypeFormatter.h>

#include <boost/container_hash/hash.hpp>

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <expected>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace ao::rt
{
  std::size_t SourceSpecHash::operator()(SourceSpec const& spec) const noexcept
  {
    auto result = std::hash<ListId>{}(spec.baseListId);
    auto const filterHash = std::hash<std::string>{}(spec.filterExpression);
    boost::hash_combine(result, filterHash);
    return result;
  }

  namespace
  {
    CachedListSourceDefinition definitionOf(ListId const listId, library::ListView const& view)
    {
      auto definition = CachedListSourceDefinition{
        .listId = listId,
        .parentId = view.parentId(),
        .expression = std::string{view.filter()},
      };

      definition.orderTrackIds.assign(view.orderTrackIds().begin(), view.orderTrackIds().end());
      return definition;
    }
  } // namespace

  TrackSourceCache::TrackSourceCache(library::MusicLibrary const& library, LibraryChanges const& changes)
    : _library{library}, _allTracksPtr{std::make_shared<AllTracksSource>(_library.tracks())}, _smartEvaluator{_library}
  {
    // The cache is the library's one replica: everything the runtime reads
    // tracks through is derived here, so it applies each revision before the
    // revision is announced to anyone else.
    _replicaBinding =
      changes.bindReplica("TrackSourceCache", [this](LibraryChangeSet const& event) { handleLibraryChange(event); });
  }

  void TrackSourceCache::handleLibraryChange(LibraryChangeSet const& event)
  {
    if (event.libraryReset)
    {
      handleLibraryReset();
      return;
    }

    applyListMutation([this, &event] { handleIncrementalLibraryChange(event); });
  }

  void TrackSourceCache::handleLibraryReset()
  {
    reloadAllTracks();
    auto liveListIds = std::vector<ListId>{};
    liveListIds.reserve(_sources.size());

    for (auto const& [listId, sourcePtr] : _sources)
    {
      std::ignore = sourcePtr;
      liveListIds.push_back(listId);
    }

    applyListMutation(
      [this, &liveListIds]
      {
        for (auto const listId : liveListIds)
        {
          refreshList(listId);
        }
      });
  }

  void TrackSourceCache::handleIncrementalLibraryChange(LibraryChangeSet const& event)
  {
    for (auto const id : event.listsDeleted)
    {
      eraseList(id);
    }

    _allTracksPtr->applyCollectionChange(event.tracksInserted, event.tracksDeleted);
    // Membership changes must reach predicate parents before an accompanying
    // raw-rank edit. Remove-from-List then publishes one final visible removal
    // instead of a transient reorder followed by departure.
    notifyMetadataUpdates(event);
    auto const detailedListIds = applyListOrderChanges(event);

    for (auto const id : event.listsUpserted)
    {
      if (!std::ranges::contains(detailedListIds, id))
      {
        refreshList(id);
      }
    }
  }

  std::vector<ListId> TrackSourceCache::applyListOrderChanges(LibraryChangeSet const& event)
  {
    auto detailedListIds = std::vector<ListId>{};
    detailedListIds.reserve(event.listOrderChanges.size());

    for (auto const& contentChange : event.listOrderChanges)
    {
      if (std::ranges::contains(event.listsDeleted, contentChange.listId))
      {
        continue;
      }

      if (!std::ranges::contains(detailedListIds, contentChange.listId))
      {
        detailedListIds.push_back(contentChange.listId);
      }

      auto sourcePtr = findSource(contentChange.listId);

      if (sourcePtr == nullptr)
      {
        continue;
      }

      std::visit(
        [&sourcePtr, listId = contentChange.listId, this](auto const& operation)
        {
          using Operation = std::remove_cvref_t<decltype(operation)>;

          if constexpr (std::same_as<Operation, delta::RegularTrackEditScript>)
          {
            sourcePtr->applyOrderEditScript(operation);
          }
          else
          {
            refreshList(listId);
          }
        },
        contentChange.operation);
    }

    return detailedListIds;
  }

  void TrackSourceCache::notifyMetadataUpdates(LibraryChangeSet const& event)
  {
    auto metadataTrackIds = std::vector<TrackId>{};
    metadataTrackIds.reserve(event.tracksMutated.size());

    for (auto const trackId : event.tracksMutated)
    {
      if (!std::ranges::contains(event.tracksInserted, trackId) && !std::ranges::contains(event.tracksDeleted, trackId))
      {
        metadataTrackIds.push_back(trackId);
      }
    }

    _allTracksPtr->applyMetadataChange(metadataTrackIds);
  }

  Result<TrackSourceLease> TrackSourceCache::acquire(ListId const listId)
  {
    return acquire(listId, {});
  }

  Result<TrackSourceLease> TrackSourceCache::acquire(SourceSpec const& spec)
  {
    if (spec.filterExpression.empty())
    {
      return acquire(spec.baseListId);
    }

    if (auto const cached = _adHocSources.find(spec); cached != _adHocSources.end())
    {
      if (auto sourcePtr = cached->second.lock(); sourcePtr != nullptr && sourcePtr->state() == TrackSourceState::Live)
      {
        return TrackSourceLease{std::move(sourcePtr)};
      }

      _adHocSources.erase(cached);
    }

    auto baseRes = acquire(spec.baseListId);

    if (!baseRes)
    {
      return std::unexpected{baseRes.error()};
    }

    auto sourcePtr = std::make_shared<SmartListSource>(*baseRes, _smartEvaluator);
    sourcePtr->setExpression(spec.filterExpression);
    sourcePtr->reload();
    auto basePtr = std::static_pointer_cast<TrackSource>(std::move(sourcePtr));
    _adHocSources.insert_or_assign(spec, basePtr);
    return TrackSourceLease{std::move(basePtr)};
  }

  std::optional<Error> TrackSourceCache::sourceError(TrackSourceLease const& lease) const
  {
    return trackSourceError(lease.source());
  }

  void TrackSourceCache::reloadAllTracks()
  {
    auto const transaction = _library.readTransaction();
    _allTracksPtr->reloadFromStore(transaction);
  }

  void TrackSourceCache::refreshList(ListId const listId)
  {
    if (listId == kInvalidListId)
    {
      return;
    }

    if (!std::ranges::contains(_pendingRefreshListIds, listId))
    {
      _pendingRefreshListIds.push_back(listId);
    }

    drainPendingRefreshes();
  }

  void TrackSourceCache::refreshListNow(ListId const listId)
  {
    auto sourcePtr = findSource(listId);

    if (sourcePtr == nullptr)
    {
      return;
    }

    auto const transaction = _library.readTransaction();
    auto const optView = _library.lists().reader(transaction).get(listId);

    if (!optView)
    {
      eraseList(listId);
      return;
    }

    auto definition = definitionOf(listId, *optView);

    if (definition == sourcePtr->definition() || sourcePtr->trySynchronizeOrderDefinition(definition))
    {
      return;
    }

    auto parentRes = acquire(resolveParentSourceId(definition.parentId));

    AO_INVARIANT(parentRes, "Failed to resolve parent source for list");

    auto implementationPtr = buildImplementation(*optView, *parentRes);
    auto const parentId = definition.parentId;
    linkGraph(listId, parentId);
    sourcePtr->rebind(std::move(definition), std::move(implementationPtr));
  }

  void TrackSourceCache::applyListMutation(compat::MoveOnlyFunction<void()> mutation)
  {
    ++_listMutationDepth;
    mutation();
    --_listMutationDepth;

    if (_listMutationDepth == 0)
    {
      drainPendingRefreshes();
    }
  }

  void TrackSourceCache::drainPendingRefreshes()
  {
    if (_listMutationDepth != 0 || _refreshDrainActive)
    {
      return;
    }

    _refreshDrainActive = true;

    while (!_pendingRefreshListIds.empty())
    {
      auto listIds = std::exchange(_pendingRefreshListIds, {});

      for (auto const listId : listIds)
      {
        refreshListNow(listId);
      }
    }

    _refreshDrainActive = false;
  }

  void TrackSourceCache::eraseList(ListId const listId)
  {
    if (isVirtualListId(listId))
    {
      return;
    }

    auto listIds = std::vector<ListId>{};
    collectDescendantsPostorder(listId, listIds);

    for (auto const id : listIds)
    {
      if (auto sourcePtr = findSource(id); sourcePtr != nullptr)
      {
        sourcePtr->semanticInvalidate();
      }

      _sources.erase(id);
      unlinkGraph(id);
    }
  }

  Result<TrackSourceLease> TrackSourceCache::acquire(ListId const listId, std::vector<ListId> ancestry)
  {
    if (listId == kAllTracksListId)
    {
      if (_allTracksPtr->state() != TrackSourceState::Live)
      {
        return makeError(Error::Code::InvalidState, "All Tracks source is unavailable");
      }

      return TrackSourceLease{_allTracksPtr};
    }

    if (listId == kInvalidListId)
    {
      return makeError(Error::Code::InvalidInput, "Invalid list id cannot be acquired as a track source");
    }

    if (std::ranges::contains(ancestry, listId))
    {
      return makeError(Error::Code::InvalidInput, "List source parent graph contains a cycle");
    }

    if (auto const it = _sources.find(listId); it != _sources.end())
    {
      if (it->second->state() == TrackSourceState::Live)
      {
        return TrackSourceLease{it->second};
      }

      eraseList(listId);
    }

    auto const transaction = _library.readTransaction();
    auto const optView = _library.lists().reader(transaction).get(listId);

    if (!optView)
    {
      return makeError(Error::Code::NotFound, std::format("List {} does not exist", listId));
    }

    ancestry.push_back(listId);
    auto parentRes = acquire(resolveParentSourceId(optView->parentId()), std::move(ancestry));

    if (!parentRes)
    {
      return std::unexpected{parentRes.error()};
    }

    auto definition = definitionOf(listId, *optView);
    auto const parentId = definition.parentId;
    auto implementationPtr = buildImplementation(*optView, *parentRes);
    auto sourcePtr = std::make_shared<CachedListSource>(std::move(definition), std::move(implementationPtr));
    _sources.insert_or_assign(listId, sourcePtr);
    linkGraph(listId, parentId);
    return TrackSourceLease{std::static_pointer_cast<TrackSource>(std::move(sourcePtr))};
  }

  std::shared_ptr<CachedListSource> TrackSourceCache::findSource(ListId const listId)
  {
    if (auto const source = _sources.find(listId); source != _sources.end())
    {
      return source->second;
    }

    return {};
  }

  std::unique_ptr<ListOrderSource> TrackSourceCache::buildImplementation(library::ListView const& view,
                                                                         TrackSourceLease const& parentLease)
  {
    auto filterSourcePtr = std::make_shared<SmartListSource>(parentLease, _smartEvaluator);
    filterSourcePtr->setExpression(std::string{view.filter()});
    filterSourcePtr->reload();
    auto filterLease = TrackSourceLease{std::static_pointer_cast<TrackSource>(std::move(filterSourcePtr))};
    auto const order = view.orderTrackIds();
    return std::make_unique<ListOrderSource>(
      std::span<TrackId const>{order.begin(), order.size()}, std::move(filterLease));
  }

  void TrackSourceCache::linkGraph(ListId const listId, ListId const parentId)
  {
    if (auto const oldParent = _parentIds.find(listId); oldParent != _parentIds.end())
    {
      if (auto children = _childIds.find(oldParent->second); children != _childIds.end())
      {
        std::erase(children->second, listId);

        if (children->second.empty())
        {
          _childIds.erase(children);
        }
      }
    }

    _parentIds.insert_or_assign(listId, parentId);

    if (parentId != kInvalidListId)
    {
      if (auto& children = _childIds[parentId]; !std::ranges::contains(children, listId))
      {
        children.push_back(listId);
      }
    }
  }

  void TrackSourceCache::unlinkGraph(ListId const listId)
  {
    if (auto const parent = _parentIds.find(listId); parent != _parentIds.end())
    {
      if (auto children = _childIds.find(parent->second); children != _childIds.end())
      {
        std::erase(children->second, listId);

        if (children->second.empty())
        {
          _childIds.erase(children);
        }
      }

      _parentIds.erase(parent);
    }

    _childIds.erase(listId);
  }

  void TrackSourceCache::collectDescendantsPostorder(ListId const listId, std::vector<ListId>& listIds) const
  {
    if (auto const children = _childIds.find(listId); children != _childIds.end())
    {
      auto const childIds = children->second;

      for (auto const childId : childIds)
      {
        collectDescendantsPostorder(childId, listIds);
      }
    }

    listIds.push_back(listId);
  }
} // namespace ao::rt
