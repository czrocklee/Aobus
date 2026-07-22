// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "AllTracksSource.h"
#include "SmartListEvaluator.h"
#include "TrackSource.h"
#include "TrackSourceLease.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Subscription.h>

#include <boost/unordered/unordered_flat_map.hpp>

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ao::library
{
  class ListView;
  class MusicLibrary;
}

namespace ao::rt
{
  struct SourceSpec final
  {
    ListId baseListId = kInvalidListId;
    std::string filterExpression{};

    bool operator==(SourceSpec const&) const = default;
  };

  struct SourceSpecHash final
  {
    std::size_t operator()(SourceSpec const& spec) const noexcept;
  };

  class LibraryChanges;
  struct LibraryChangeSet;
  class CachedListSource;

  class TrackSourceCache final
  {
  public:
    TrackSourceCache(library::MusicLibrary const& library, LibraryChanges const& changes);
    ~TrackSourceCache() = default;

    TrackSourceCache(TrackSourceCache const&) = delete;
    TrackSourceCache& operator=(TrackSourceCache const&) = delete;
    TrackSourceCache(TrackSourceCache&&) = delete;
    TrackSourceCache& operator=(TrackSourceCache&&) = delete;

    Result<TrackSourceLease> acquire(ListId listId);
    Result<TrackSourceLease> acquire(SourceSpec const& spec);
    std::optional<Error> sourceError(TrackSourceLease const& lease) const;

    void reloadAllTracks();

  private:
    Result<TrackSourceLease> acquire(ListId listId, std::vector<ListId> ancestry);
    void handleLibraryChange(LibraryChangeSet const& event);
    void handleLibraryReset();
    void handleIncrementalLibraryChange(LibraryChangeSet const& event);
    std::vector<ListId> applyManualContentChanges(LibraryChangeSet const& event);
    void notifyMetadataUpdates(LibraryChangeSet const& event);
    void refreshList(ListId listId);
    void eraseList(ListId listId);
    void applyListMutation(std::move_only_function<void()> mutation);
    void drainPendingRefreshes();
    void refreshListNow(ListId listId);
    std::shared_ptr<CachedListSource> findSource(ListId listId);
    std::unique_ptr<TrackSource> buildImplementation(library::ListView const& view,
                                                     TrackSourceLease const& parentLease);
    void linkGraph(ListId listId, ListId parentId);
    void unlinkGraph(ListId listId);
    void collectDescendantsPostorder(ListId listId, std::vector<ListId>& listIds) const;

    library::MusicLibrary const& _library;
    std::shared_ptr<AllTracksSource> _allTracksPtr;
    SmartListEvaluator _smartEvaluator;

    async::Subscription _changesSubscription;

    std::size_t _listMutationDepth = 0;
    bool _refreshDrainActive = false;
    std::vector<ListId> _pendingRefreshListIds;

    boost::unordered_flat_map<ListId, std::shared_ptr<CachedListSource>, std::hash<ListId>> _sources;
    boost::unordered_flat_map<ListId, ListId, std::hash<ListId>> _parentIds;
    boost::unordered_flat_map<ListId, std::vector<ListId>, std::hash<ListId>> _childIds;
    boost::unordered_flat_map<SourceSpec, std::weak_ptr<TrackSource>, SourceSpecHash> _adHocSources;
  };
} // namespace ao::rt
