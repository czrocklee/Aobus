// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "AllTracksSource.h"
#include "SmartListEvaluator.h"
#include "TrackSource.h"

#include <ao/Type.h>
#include <ao/library/MusicLibrary.h>

#include <memory>
#include <unordered_map>

namespace ao::app
{
  class ListSourceStore final
  {
  public:
    explicit ListSourceStore(ao::library::MusicLibrary& library);
    ~ListSourceStore();

    ListSourceStore(ListSourceStore const&) = delete;
    ListSourceStore& operator=(ListSourceStore const&) = delete;

    ao::app::TrackSource& allTracks();
    ao::app::TrackSource& sourceFor(ao::ListId listId);

    void reloadAllTracks();
    void refreshList(ao::ListId listId);
    void eraseList(ao::ListId listId);

    ao::app::SmartListEvaluator& smartEvaluator() { return _smartEvaluator; }

  private:
    TrackSource& getOrBuildSource(ao::ListId listId);

    ao::library::MusicLibrary& _library;
    AllTracksSource _allTracks;
    SmartListEvaluator _smartEvaluator;

    std::unordered_map<ao::ListId, std::unique_ptr<TrackSource>> _sources;
  };
}
