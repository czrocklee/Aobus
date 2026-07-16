// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "../Subscription.h"
#include "AllTracksSource.h"
#include "SmartListEvaluator.h"
#include "TrackSource.h"
#include "TrackSourceLease.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>

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

    TrackSource& allTracks();
    Result<TrackSourceLease> acquire(ListId listId);
    Result<TrackSourceLease> acquire(SourceSpec const& spec);
    std::optional<Error> sourceError(TrackSourceLease const& lease) const;

    void reloadAllTracks();
    void refreshList(ListId listId);
    void evict(ListId listId);
    void eraseList(ListId listId);

    SmartListEvaluator& smartEvaluator();

  private:
    Result<TrackSourceLease> acquire(ListId listId, std::vector<ListId> ancestry);
    void applyListMutation(std::move_only_function<void()> mutation);
    void drainPendingRefreshes();
    void refreshListNow(ListId listId);
    std::shared_ptr<CachedListSource> liveSource(ListId listId);
    std::unique_ptr<TrackSource> buildImplementation(library::ListView const& view,
                                                     TrackSourceLease const& parentLease);
    void linkGraph(ListId listId, ListId parentId);
    void unlinkGraph(ListId listId);
    void collectDescendantsPostorder(ListId listId, std::vector<ListId>& listIds) const;

    library::MusicLibrary const& _library;
    std::shared_ptr<AllTracksSource> _allTracksPtr;
    SmartListEvaluator _smartEvaluator;

    Subscription _changesSubscription;

    std::size_t _listMutationDepth = 0;
    bool _refreshDrainActive = false;
    std::vector<ListId> _pendingRefreshListIds;

    boost::unordered_flat_map<ListId, std::shared_ptr<CachedListSource>, std::hash<ListId>> _hotSources;
    boost::unordered_flat_map<ListId, std::weak_ptr<CachedListSource>, std::hash<ListId>> _liveSources;
    boost::unordered_flat_map<ListId, ListId, std::hash<ListId>> _parentIds;
    boost::unordered_flat_map<ListId, std::vector<ListId>, std::hash<ListId>> _childIds;
    boost::unordered_flat_map<SourceSpec, std::weak_ptr<TrackSource>, SourceSpecHash> _adHocSources;
  };
} // namespace ao::rt
