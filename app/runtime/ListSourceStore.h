// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "AllTracksSource.h"
#include "CorePrimitives.h"
#include "SmartListEvaluator.h"
#include "TrackSource.h"

#include <ao/Type.h>
#include <ao/library/MusicLibrary.h>

#include <memory>
#include <unordered_map>

namespace ao::rt
{
  class LibraryMutationService;

  class ListSourceStore final
  {
  public:
    ListSourceStore(library::MusicLibrary& library, LibraryMutationService& mutation);
    ~ListSourceStore();

    ListSourceStore(ListSourceStore const&) = delete;
    ListSourceStore& operator=(ListSourceStore const&) = delete;

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

    std::unordered_map<ListId, std::unique_ptr<TrackSource>> _sources;
  };
}
