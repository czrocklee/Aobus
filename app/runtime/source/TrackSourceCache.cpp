// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "runtime/source/CachedListSource.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/Exception.h>
#include <ao/library/ListStore.h>
#include <ao/library/ListView.h>
#include <ao/library/MusicLibrary.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/source/AllTracksSource.h>
#include <ao/rt/source/ManualListSource.h>
#include <ao/rt/source/SmartListSource.h>
#include <ao/rt/source/TrackSource.h>
#include <ao/rt/source/TrackSourceCache.h>
#include <ao/rt/source/TrackSourceLease.h>

#include <boost/container_hash/hash.hpp>

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <exception>
#include <expected>
#include <format>
#include <functional>
#include <memory>
#include <optional>
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
    CachedListSourceDefinition definitionOf(library::ListView const& view)
    {
      auto definition = CachedListSourceDefinition{
        .parentId = view.parentId(),
        .kind = view.isSmart() ? CachedListSourceKind::Smart : CachedListSourceKind::Manual,
      };

      if (view.isSmart())
      {
        definition.smartExpression = view.filter();
      }
      else
      {
        definition.storedTrackIds.assign(view.tracks().begin(), view.tracks().end());
      }

      return definition;
    }
  } // namespace

  TrackSourceCache::TrackSourceCache(library::MusicLibrary const& library, LibraryChanges const& changes)
    : _library{library}, _allTracksPtr{std::make_shared<AllTracksSource>(_library.tracks())}, _smartEvaluator{_library}
  {
    _changesSubscription = changes.onChanged([this](LibraryChangeSet const& event) { handleLibraryChange(event); });
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
    liveListIds.reserve(_liveSources.size());

    for (auto const& [listId, sourcePtr] : _liveSources)
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
    auto const detailedListIds = applyManualContentChanges(event);

    for (auto const id : event.listsUpserted)
    {
      if (!std::ranges::contains(detailedListIds, id))
      {
        refreshList(id);
      }
    }

    notifyMetadataUpdates(event);
  }

  std::vector<ListId> TrackSourceCache::applyManualContentChanges(LibraryChangeSet const& event)
  {
    auto detailedListIds = std::vector<ListId>{};
    detailedListIds.reserve(event.manualContentChanges.size());

    for (auto const& contentChange : event.manualContentChanges)
    {
      if (std::ranges::contains(event.listsDeleted, contentChange.listId))
      {
        continue;
      }

      if (!std::ranges::contains(detailedListIds, contentChange.listId))
      {
        detailedListIds.push_back(contentChange.listId);
      }

      auto sourcePtr = liveSource(contentChange.listId);

      if (sourcePtr == nullptr)
      {
        continue;
      }

      std::visit(
        [&sourcePtr, listId = contentChange.listId, this](auto const& operation)
        {
          using Operation = std::remove_cvref_t<decltype(operation)>;

          if constexpr (std::same_as<Operation, ManualTracksInsert>)
          {
            sourcePtr->applyManualTracksInsert(operation);
          }
          else if constexpr (std::same_as<Operation, ManualTracksRemove>)
          {
            sourcePtr->applyManualTracksRemove(operation);
          }
          else if constexpr (std::same_as<Operation, ManualTracksMove>)
          {
            sourcePtr->applyManualTracksMove(operation);
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

    _allTracksPtr->notifyUpdated(metadataTrackIds);
  }

  TrackSource& TrackSourceCache::allTracks()
  {
    return *_allTracksPtr;
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

    auto baseResult = acquire(spec.baseListId);

    if (!baseResult)
    {
      return std::unexpected{baseResult.error()};
    }

    auto sourcePtr = std::make_shared<SmartListSource>(*baseResult, _smartEvaluator);
    sourcePtr->setExpression(spec.filterExpression);
    sourcePtr->reload();
    auto basePtr = std::static_pointer_cast<TrackSource>(std::move(sourcePtr));
    _adHocSources.insert_or_assign(spec, basePtr);
    return TrackSourceLease{std::move(basePtr)};
  }

  std::optional<Error> TrackSourceCache::sourceError(TrackSourceLease const& lease) const
  {
    if (auto const* source = dynamic_cast<SmartListSource const*>(&lease.source()); source != nullptr)
    {
      return source->error();
    }

    return std::nullopt;
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
    auto sourcePtr = liveSource(listId);

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

    auto definition = definitionOf(*optView);

    if (definition == sourcePtr->definition() || sourcePtr->trySynchronizeManualDefinition(definition))
    {
      return;
    }

    auto parentResult =
      definition.parentId == kInvalidListId ? acquire(kAllTracksListId) : acquire(definition.parentId);

    if (!parentResult)
    {
      throwException<Exception>(
        "Failed to resolve parent source for list {}: {}", listId, parentResult.error().message);
    }

    auto implementationPtr = buildImplementation(*optView, *parentResult);
    auto const parentId = definition.parentId;
    linkGraph(listId, parentId);

    try
    {
      sourcePtr->rebind(std::move(definition), *parentResult, std::move(implementationPtr));
    }
    catch (...)
    {
      if (sourcePtr->state() == TrackSourceState::Live)
      {
        linkGraph(listId, sourcePtr->definition().parentId);
      }
      else
      {
        unlinkGraph(listId);
      }

      throw;
    }
  }

  void TrackSourceCache::applyListMutation(std::move_only_function<void()> mutation)
  {
    ++_listMutationDepth;
    auto optException = std::exception_ptr{};

    try
    {
      mutation();
    }
    catch (...)
    {
      optException = std::current_exception();
    }

    --_listMutationDepth;

    if (_listMutationDepth == 0)
    {
      try
      {
        drainPendingRefreshes();
      }
      catch (...)
      {
        if (optException == nullptr)
        {
          optException = std::current_exception();
        }
      }
    }

    if (optException != nullptr)
    {
      std::rethrow_exception(optException);
    }
  }

  void TrackSourceCache::drainPendingRefreshes()
  {
    if (_listMutationDepth != 0 || _refreshDrainActive)
    {
      return;
    }

    _refreshDrainActive = true;
    auto optException = std::exception_ptr{};

    while (!_pendingRefreshListIds.empty())
    {
      auto listIds = std::exchange(_pendingRefreshListIds, {});

      for (auto const listId : listIds)
      {
        try
        {
          refreshListNow(listId);
        }
        catch (...)
        {
          if (optException == nullptr)
          {
            optException = std::current_exception();
          }
        }
      }
    }

    _refreshDrainActive = false;

    if (optException != nullptr)
    {
      std::rethrow_exception(optException);
    }
  }

  void TrackSourceCache::evict(ListId const listId)
  {
    if (listId != kAllTracksListId)
    {
      _hotSources.erase(listId);
    }
  }

  void TrackSourceCache::eraseList(ListId const listId)
  {
    if (listId == kInvalidListId || listId == kAllTracksListId)
    {
      return;
    }

    auto listIds = std::vector<ListId>{};
    collectDescendantsPostorder(listId, listIds);

    for (auto const id : listIds)
    {
      if (auto sourcePtr = liveSource(id); sourcePtr != nullptr)
      {
        sourcePtr->semanticInvalidate();
      }

      _hotSources.erase(id);
      _liveSources.erase(id);
      unlinkGraph(id);
    }
  }

  SmartListEvaluator& TrackSourceCache::smartEvaluator()
  {
    return _smartEvaluator;
  }

  Result<TrackSourceLease> TrackSourceCache::acquire(ListId const listId, std::vector<ListId> ancestry)
  {
    if (listId == kAllTracksListId)
    {
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

    if (auto const it = _hotSources.find(listId); it != _hotSources.end())
    {
      return TrackSourceLease{it->second};
    }

    if (auto sourcePtr = liveSource(listId); sourcePtr != nullptr)
    {
      _hotSources.insert_or_assign(listId, sourcePtr);
      return TrackSourceLease{std::move(sourcePtr)};
    }

    auto const transaction = _library.readTransaction();
    auto const optView = _library.lists().reader(transaction).get(listId);

    if (!optView)
    {
      return makeError(Error::Code::NotFound, std::format("List {} does not exist", listId));
    }

    ancestry.push_back(listId);
    auto parentResult = optView->parentId() == kInvalidListId ? acquire(kAllTracksListId, std::move(ancestry))
                                                              : acquire(optView->parentId(), std::move(ancestry));

    if (!parentResult)
    {
      return std::unexpected{parentResult.error()};
    }

    auto definition = definitionOf(*optView);
    auto const parentId = definition.parentId;
    auto implementationPtr = buildImplementation(*optView, *parentResult);
    auto sourcePtr =
      std::make_shared<CachedListSource>(listId, std::move(definition), *parentResult, std::move(implementationPtr));
    _hotSources.insert_or_assign(listId, sourcePtr);
    _liveSources.insert_or_assign(listId, sourcePtr);
    linkGraph(listId, parentId);
    return TrackSourceLease{std::static_pointer_cast<TrackSource>(std::move(sourcePtr))};
  }

  std::shared_ptr<CachedListSource> TrackSourceCache::liveSource(ListId const listId)
  {
    if (auto const hot = _hotSources.find(listId); hot != _hotSources.end())
    {
      return hot->second;
    }

    auto const live = _liveSources.find(listId);

    if (live == _liveSources.end())
    {
      return {};
    }

    if (auto sourcePtr = live->second.lock(); sourcePtr != nullptr)
    {
      return sourcePtr;
    }

    _liveSources.erase(live);
    unlinkGraph(listId);
    return {};
  }

  std::unique_ptr<TrackSource> TrackSourceCache::buildImplementation(library::ListView const& view,
                                                                     TrackSourceLease const& parentLease)
  {
    if (view.isSmart())
    {
      auto sourcePtr = std::make_unique<SmartListSource>(parentLease, _smartEvaluator);
      sourcePtr->setExpression(std::string{view.filter()});
      sourcePtr->reload();
      return sourcePtr;
    }

    return std::make_unique<ManualListSource>(view, parentLease);
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
