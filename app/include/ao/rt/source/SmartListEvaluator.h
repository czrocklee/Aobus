// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "TrackSource.h"
#include <ao/CoreIds.h>

#include <boost/unordered/unordered_flat_map.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace ao::library
{
  class MusicLibrary;
}

namespace ao::rt
{
  class SmartListSource;
  class SmartListEvaluator;

  // SourceObserver - handles source list events and dispatches to evaluator
  class SourceObserver final : public TrackSourceObserver
  {
  public:
    explicit SourceObserver(SmartListEvaluator& evaluator, TrackSource& source);

    void onReset() override;
    void onInserted(TrackId id, std::size_t index) override;
    void onUpdated(TrackId id, std::size_t index) override;
    void onRemoved(TrackId id, std::size_t index) override;

    void onBulkInserted(std::span<TrackId const> ids) override;
    void onBulkUpdated(std::span<TrackId const> ids) override;
    void onBulkRemoved(std::span<TrackId const> ids) override;

    void onSourceDestroyed() override;

    void invalidate() { _valid = false; }

  private:
    SmartListEvaluator& _evaluator;
    TrackSource& _source;
    bool _valid = true;
  };

  /**
   * SmartListEvaluator - Batching expression evaluator for smart list filtering.
   *
   * The evaluator coordinates batch evaluation of SmartListSources that share the same source.
   */
  class SmartListEvaluator final
  {
  public:
    explicit SmartListEvaluator(library::MusicLibrary& ml);
    ~SmartListEvaluator();

    // Disable copy/move
    SmartListEvaluator(SmartListEvaluator const&) = delete;
    SmartListEvaluator& operator=(SmartListEvaluator const&) = delete;
    SmartListEvaluator(SmartListEvaluator&&) = delete;
    SmartListEvaluator& operator=(SmartListEvaluator&&) = delete;

    bool isAlive() const { return _alive; }

    void registerList(TrackSource& source, SmartListSource& list);
    void unregisterList(TrackSource& source, SmartListSource& list);

    void rebuild(SmartListSource& list);

    // Notify evaluator that a track's data changed so it can re-evaluate filter membership
    void notifyUpdated(TrackSource& source, TrackId trackId);

  private:
    enum class TrackLoadMode : std::uint8_t
    {
      Hot,
      Cold,
      Both,
    };

    struct SourceBucket final
    {
      TrackSource* source = nullptr;
      bool sourceAlive = true;
      std::vector<SmartListSource*> lists;
      std::unique_ptr<TrackSourceObserver> observerPtr;
    };

    void evaluateAllLists(SourceBucket& bucket);
    void evaluateDirtyLists(SourceBucket& bucket);
    void evaluateLists(std::span<SmartListSource*> lists);
    void evaluateMembers(TrackSource& source, std::span<SmartListSource*> lists, TrackLoadMode mode);

    void handleSourceReset(SourceBucket& bucket);
    void handleSourceInserted(SourceBucket& bucket, TrackId id, std::size_t sourceIndex);
    void handleSourceUpdated(SourceBucket& bucket, TrackId id, std::size_t sourceIndex);
    void handleSourceRemoved(SourceBucket& bucket, TrackId id);

    void handleSourceInserted(SourceBucket& bucket, std::span<TrackId const> ids);
    void handleSourceUpdated(SourceBucket& bucket, std::span<TrackId const> ids);
    void handleSourceRemoved(SourceBucket& bucket, std::span<TrackId const> ids);

    void handleSourceDestroyed(SourceBucket& bucket);

    static TrackLoadMode unionMode(std::span<SmartListSource*> lists);

    library::MusicLibrary& _ml;
    boost::unordered_flat_map<TrackSource*, std::unique_ptr<SourceBucket>> _buckets;
    bool _alive = true;

    friend class SourceObserver;
    friend class SmartListSource;
  };
} // namespace ao::rt
