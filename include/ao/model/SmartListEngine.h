// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/model/TrackIdList.h>

#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackStore.h>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ao::model
{
  class FilteredTrackIdList;
  class SmartListEngine;

  // SourceObserver - handles source list events and dispatches to engine
  class SourceObserver final : public TrackIdListObserver
  {
  public:
    explicit SourceObserver(SmartListEngine& engine, TrackIdList& source);

    void onReset() override;
    void onInserted(TrackId id, std::size_t index) override;
    void onUpdated(TrackId id, std::size_t index) override;
    void onRemoved(TrackId id, std::size_t index) override;

    void onInserted(std::span<TrackId const> ids) override;
    void onUpdated(std::span<TrackId const> ids) override;
    void onRemoved(std::span<TrackId const> ids) override;

    void onSourceDestroyed() override;

    void invalidate() { _valid = false; }

  private:
    SmartListEngine& _engine;
    TrackIdList& _source;
    bool _valid = true;
  };

  /**
   * SmartListEngine - Batching expression evaluator for smart list filtering.
   *
   * The engine no longer stores membership data. Instead, it coordinates
   * batch evaluation of FilteredTrackIdLists that share the same source.
   */
  class SmartListEngine final
  {
  public:
    explicit SmartListEngine(ao::library::MusicLibrary& ml);
    ~SmartListEngine();

    // Disable copy/move
    SmartListEngine(SmartListEngine const&) = delete;
    SmartListEngine& operator=(SmartListEngine const&) = delete;
    SmartListEngine(SmartListEngine&&) = delete;
    SmartListEngine& operator=(SmartListEngine&&) = delete;

    bool isAlive() const { return _alive; }

    void registerList(TrackIdList& source, FilteredTrackIdList& list);
    void unregisterList(TrackIdList& source, FilteredTrackIdList& list);

    void rebuild(FilteredTrackIdList& list);

    // Notify engine that a track's data changed so it can re-evaluate filter membership
    void notifyUpdated(TrackIdList& source, TrackId trackId);

  private:
    struct SourceBucket
    {
      TrackIdList* source = nullptr;
      bool sourceAlive = true;
      std::vector<FilteredTrackIdList*> lists;
      std::unique_ptr<TrackIdListObserver> observer;
    };

    void rebuildActiveLists(SourceBucket& bucket);
    void rebuildDirtyLists(SourceBucket& bucket);
    void rebuildLists(std::span<FilteredTrackIdList*> lists);
    void rebuildGroup(TrackIdList& source,
                      std::span<FilteredTrackIdList*> lists,
                      ao::library::TrackStore::Reader::LoadMode mode);

    void handleSourceReset(SourceBucket& bucket);
    void handleSourceInserted(SourceBucket& bucket, TrackId id, std::size_t sourceIndex);
    void handleSourceUpdated(SourceBucket& bucket, TrackId id, std::size_t sourceIndex);
    void handleSourceRemoved(SourceBucket& bucket, TrackId id);

    void handleSourceInserted(SourceBucket& bucket, std::span<TrackId const> ids);
    void handleSourceUpdated(SourceBucket& bucket, std::span<TrackId const> ids);
    void handleSourceRemoved(SourceBucket& bucket, std::span<TrackId const> ids);

    void handleSourceDestroyed(SourceBucket& bucket);

    static ao::library::TrackStore::Reader::LoadMode getUnionMode(std::span<FilteredTrackIdList*> lists);

    ao::library::MusicLibrary& _ml;
    std::map<TrackIdList*, std::unique_ptr<SourceBucket>> _buckets;
    bool _alive = true;

    friend class SourceObserver;
    friend class FilteredTrackIdList;
  };
} // namespace ao::model
