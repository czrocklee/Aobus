// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "runtime/source/IndexedTrackSequence.h"
#include <ao/CoreIds.h>
#include <ao/async/Subscription.h>
#include <ao/rt/TrackEditScript.h>
#include <ao/rt/source/TrackSourceDelta.h>

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

namespace ao::query
{
  enum class AccessProfile : std::uint8_t;
}

namespace ao::rt
{
  namespace detail
  {
    class RuntimeOperationProbe;
  }

  class SmartListSource;
  class TrackSource;

  /**
   * Batches expression evaluation for every SmartListSource sharing an
   * upstream TrackSource.
   *
   * This evaluator is a source-pipeline implementation detail; callers acquire
   * the resulting TrackSource through TrackSourceCache.
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
    void registerList(SmartListSource& list);
    void unregisterList(SmartListSource& list);
    void rebuild(SmartListSource& list);

  private:
    struct SourceBucket final
    {
      TrackSource const* source = nullptr;
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
      delta::RegularTrackEditScript script{};
      bool active = false;
    };

    using TrackMatches = boost::unordered_flat_map<TrackId, std::vector<bool>, std::hash<TrackId>>;

    void handleSourceBatch(TrackSource const& source, TrackSourceDelta const& batch);
    void handleSourceReset(SourceBucket& bucket);
    void handleRegularBatch(SourceBucket& bucket,
                            delta::RegularTrackEditScript const& script,
                            bool verifyFinalSnapshot = true);
    void handleUpdateBatch(SourceBucket& bucket, delta::RegularTrackEditScript const& script, bool verifyFinalSnapshot);
    void handleSourceInvalidated(SourceBucket& bucket);

    std::vector<DerivedWork> buildDerivedWorks(SourceBucket const& bucket) const;
    TrackMatches evaluateTouchedTracks(std::span<SmartListSource* const> lists,
                                       std::span<TrackId const> touchedTrackIds) const;
    delta::RegularTrackEditScript buildUpdateScript(SmartListSource const& list,
                                                    std::size_t listIndex,
                                                    std::span<TrackId const> updatedTrackIds,
                                                    TrackMatches const& matchesByTrackId,
                                                    IndexedTrackSequence const& upstreamTracks) const;
    static void updateDerivedWorks(std::span<DerivedWork> works,
                                   IndexedTrackSequence const& upstreamTracks,
                                   TrackMatches const& matchesByTrackId,
                                   std::span<TrackId const> updatedTrackIds,
                                   std::span<TrackId const> preferredMovedIds);
    void commitDerivedWorks(SourceBucket& bucket, IndexedTrackSequence upstreamTracks, std::vector<DerivedWork>& works);

    void evaluatePendingLists(SourceBucket& bucket);
    void rebuildLists(SourceBucket& bucket, std::span<SmartListSource*> lists);

    static bool isEvaluatable(SmartListSource const& list);
    static query::AccessProfile unionAccessProfile(std::span<SmartListSource* const> lists);

    library::MusicLibrary const& _ml;
    boost::unordered_flat_map<TrackSource const*, std::unique_ptr<SourceBucket>> _buckets;
    bool _alive = true;
    std::size_t _upstreamIndexRebuildCount = 0;
    std::size_t _membershipIndexRebuildCount = 0;

    friend class SmartListSource;
    friend class detail::RuntimeOperationProbe;
  };
} // namespace ao::rt
