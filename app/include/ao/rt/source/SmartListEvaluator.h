// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "TrackSourceDelta.h"
#include <ao/CoreIds.h>
#include <ao/rt/Subscription.h>

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
    explicit SmartListEvaluator(library::MusicLibrary& ml);
    ~SmartListEvaluator();

    SmartListEvaluator(SmartListEvaluator const&) = delete;
    SmartListEvaluator& operator=(SmartListEvaluator const&) = delete;
    SmartListEvaluator(SmartListEvaluator&&) = delete;
    SmartListEvaluator& operator=(SmartListEvaluator&&) = delete;

    bool isAlive() const noexcept { return _alive; }

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

    using TrackIndex = boost::unordered_flat_map<TrackId, std::size_t, std::hash<TrackId>>;

    struct SourceBucket final
    {
      TrackSource* source = nullptr;
      std::vector<TrackId> upstreamTrackIds{};
      TrackIndex upstreamIndex{};
      std::vector<SmartListSource*> lists{};
      Subscription subscription{};
      bool invalidated = false;
    };

    struct DerivedWork final
    {
      SmartListSource* list = nullptr;
      std::vector<TrackId> oldMembers{};
      std::vector<TrackId> members{};
      TrackIndex memberIndex{};
      boost::container::small_vector<TrackSourceDelta, 1> deltas{};
      bool active = false;
    };

    void handleSourceBatch(TrackSource& source, TrackSourceDeltaBatch const& batch);
    void handleSourceReset(SourceBucket& bucket);
    void handleRegularBatch(SourceBucket& bucket, TrackSourceDeltaBatch const& batch, bool verifyFinalSnapshot = true);
    void handleSourceInvalidated(SourceBucket& bucket);

    void evaluateDirtyLists(SourceBucket& bucket);
    void rebuildLists(SourceBucket& bucket, std::span<SmartListSource*> lists);

    static TrackLoadMode unionMode(std::span<SmartListSource* const> lists);

    library::MusicLibrary& _ml;
    boost::unordered_flat_map<TrackSource*, std::unique_ptr<SourceBucket>> _buckets;
    bool _alive = true;

    friend class SmartListSource;
  };
} // namespace ao::rt
