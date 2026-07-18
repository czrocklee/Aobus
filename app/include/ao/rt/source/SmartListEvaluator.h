// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "IndexedTrackSequence.h"
#include "TrackSourceDelta.h"
#include <ao/CoreIds.h>
#include <ao/async/Subscription.h>

#include <boost/container/small_vector.hpp>
#include <boost/unordered/unordered_flat_map.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace ao::library
{
  class MusicLibrary;
}

namespace ao::rt
{
  struct SmartListEvaluatorOperationCounts final
  {
    std::size_t upstreamIndexRebuilds = 0;
    std::size_t membershipIndexRebuilds = 0;
  };

  class SmartListSource;
  class TrackSource;

  /**
   * Batches expression evaluation for every SmartListSource sharing an
   * upstream TrackSource.
   *
   * Each source bucket owns an ordered mirror of the upstream IDs. A complete
   * upstream delta batch is replayed into that mirror before any dependent
   * notification is published, so every derived list emits at most one atomic
   * batch and remains a stable subsequence of upstream order.
   */
  class SmartListEvaluator final
  {
  public:
    explicit SmartListEvaluator(library::MusicLibrary const& ml);
    ~SmartListEvaluator();

    SmartListEvaluator(SmartListEvaluator const&) = delete;
    SmartListEvaluator& operator=(SmartListEvaluator const&) = delete;
    SmartListEvaluator(SmartListEvaluator&&) = delete;
    SmartListEvaluator& operator=(SmartListEvaluator&&) = delete;

    bool isAlive() const noexcept { return _alive; }
    SmartListEvaluatorOperationCounts operationCounts() const noexcept { return _operationCounts; }

    void registerList(SmartListSource& list);
    void unregisterList(SmartListSource& list);
    void rebuild(SmartListSource& list);

    // Re-evaluates one upstream member after a metadata-only mutation.
    void notifyUpdated(SmartListSource& list, TrackId trackId);

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
      IndexedTrackSequence upstreamTracks{};
      std::vector<SmartListSource*> lists{};
      async::Subscription subscription{};
      bool invalidated = false;
    };

    struct DerivedWork final
    {
      SmartListSource* list = nullptr;
      std::vector<TrackId> oldMembers{};
      std::vector<TrackId> members{};
      boost::container::small_vector<TrackSourceDelta, 1> deltas{};
      bool active = false;
    };

    using TrackMatches = boost::unordered_flat_map<TrackId, std::vector<bool>, std::hash<TrackId>>;

    void handleSourceBatch(TrackSource& source, TrackSourceDeltaBatch const& batch);
    void handleSourceReset(SourceBucket& bucket);
    void handleRegularBatch(SourceBucket& bucket, TrackSourceDeltaBatch const& batch, bool verifyFinalSnapshot = true);
    void handleSourceInvalidated(SourceBucket& bucket);

    std::vector<DerivedWork> buildDerivedWorks(SourceBucket const& bucket,
                                               std::vector<SmartListSource*>& evaluatableLists) const;
    TrackMatches evaluateTouchedTracks(std::span<DerivedWork const> works,
                                       std::span<SmartListSource* const> evaluatableLists,
                                       std::span<TrackId const> touchedTrackIds) const;
    static void updateDerivedWorks(std::span<DerivedWork> works,
                                   IndexedTrackSequence const& upstreamTracks,
                                   TrackMatches const& matchesByTrackId,
                                   std::span<TrackId const> updatedTrackIds,
                                   std::span<TrackId const> preferredMovedIds);
    void commitDerivedWorks(SourceBucket& bucket, IndexedTrackSequence upstreamTracks, std::vector<DerivedWork>& works);

    void evaluateDirtyLists(SourceBucket& bucket);
    void rebuildLists(SourceBucket& bucket, std::span<SmartListSource*> lists);

    static TrackLoadMode unionMode(std::span<SmartListSource* const> lists);

    library::MusicLibrary const& _ml;
    boost::unordered_flat_map<TrackSource*, std::unique_ptr<SourceBucket>> _buckets;
    bool _alive = true;
    SmartListEvaluatorOperationCounts _operationCounts;

    friend class SmartListSource;
  };
} // namespace ao::rt
