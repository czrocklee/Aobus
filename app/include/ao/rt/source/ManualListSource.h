// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "TrackSource.h"
#include "TrackSourceLease.h"
#include <ao/CoreIds.h>
#include <ao/rt/Subscription.h>

#include <boost/unordered/unordered_flat_map.hpp>

#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <vector>

namespace ao::library
{
  class ListView;
}

namespace ao::rt
{
  struct ManualStoredRemoveRange;
  struct ManualTracksInsert;
  struct ManualTracksMove;
  struct ManualTracksRemove;

  /**
   * A manually ordered source whose stored intent is filtered by a parent.
   *
   * The stored order is canonical and independent from parent visibility. The
   * effective TrackSource snapshot is the stored order intersected with the
   * current parent membership, so a temporarily hidden track returns to its
   * original manual position when it re-enters the parent.
   */
  class ManualListSource final : public TrackSource
  {
  public:
    ManualListSource(library::ListView const& view, TrackSourceLease parentLease);
    ~ManualListSource() override;

    ManualListSource(ManualListSource const&) = delete;
    ManualListSource& operator=(ManualListSource const&) = delete;
    ManualListSource(ManualListSource&&) = delete;
    ManualListSource& operator=(ManualListSource&&) = delete;

    void reloadFromListView(library::ListView const& view);

    // Internal exact-operation entry points used by TrackSourceCache.
    void applyManualTracksInsert(ManualTracksInsert const& operation);
    void applyManualTracksRemove(ManualTracksRemove const& operation);
    void applyManualTracksMove(ManualTracksMove const& operation);

    std::span<TrackId const> storedTrackIds() const noexcept { return _storedTrackIds; }

    std::size_t size() const override { return _effectiveTrackIds.size(); }
    TrackId trackIdAt(std::size_t index) const override { return _effectiveTrackIds.at(index); }
    std::optional<std::size_t> indexOf(TrackId id) const override;

    bool contains(TrackId id) const;

  private:
    using TrackIndexMap = boost::unordered_flat_map<TrackId, std::size_t, std::hash<TrackId>>;

    void ensureLive() const;
    void loadStoredTracks(library::ListView const& view);
    void rebuildStoredIndex();
    void rebuildEffectiveTracks();
    std::vector<TrackId> validateStoredRemovals(std::span<ManualStoredRemoveRange const> removals) const;
    void eraseStoredRemovals(std::span<ManualStoredRemoveRange const> removals);
    void publishVisibilityDelta(std::vector<TrackId> const& previousEffective,
                                std::span<TrackId const> updatedTrackIds = {});
    void publishExactMoveDelta(std::vector<TrackId> const& previousEffective, std::span<TrackId const> movedTrackIds);
    void handleParentBatch(TrackSourceDeltaBatch const& batch);

    TrackSourceLease _parentLease;
    std::vector<TrackId> _storedTrackIds;
    TrackIndexMap _storedIndexByTrackId;
    std::vector<TrackId> _effectiveTrackIds;
    TrackIndexMap _effectiveIndexByTrackId;
    Subscription _parentSubscription;
  };
} // namespace ao::rt
