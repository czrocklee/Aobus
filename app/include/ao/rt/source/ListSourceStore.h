// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "../Subscription.h"
#include "AllTracksSource.h"
#include "SmartListEvaluator.h"
#include "TrackSource.h"
#include <ao/CoreIds.h>

#include <boost/unordered/unordered_flat_map.hpp>

#include <functional>
#include <memory>

namespace ao::library
{
  class MusicLibrary;
}

namespace ao::rt
{
  class LibraryChanges;

  class ListSourceStore final
  {
  public:
    ListSourceStore(library::MusicLibrary& library, LibraryChanges const& changes);
    ~ListSourceStore();

    ListSourceStore(ListSourceStore const&) = delete;
    ListSourceStore& operator=(ListSourceStore const&) = delete;
    ListSourceStore(ListSourceStore&&) = delete;
    ListSourceStore& operator=(ListSourceStore&&) = delete;

    TrackSource& allTracks();
    TrackSource& sourceFor(ListId listId);

    void reloadAllTracks();
    void refreshList(ListId listId);
    void eraseList(ListId listId);

    SmartListEvaluator& smartEvaluator() { return _smartEvaluator; }

  private:
    TrackSource& getOrBuildSource(ListId listId);

    library::MusicLibrary& _library;
    AllTracksSource _allTracks;
    SmartListEvaluator _smartEvaluator;

    Subscription _listsMutatedSubscription;
    Subscription _trackCollectionChangedSubscription;

    boost::unordered_flat_map<ListId, std::unique_ptr<TrackSource>, std::hash<ListId>> _sources;
  };
} // namespace ao::rt
