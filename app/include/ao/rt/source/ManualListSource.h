// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "IndexedTrackSequence.h"
#include "TrackSource.h"
#include "TrackSourceLease.h"
#include <ao/CoreIds.h>
#include <ao/async/Subscription.h>

#include <cstddef>
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
   * A manually ordered source whose stored rows are filtered by a parent.
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

    std::span<TrackId const> storedTrackIds() const noexcept { return _storedTracks.ids(); }

    std::size_t size() const override { return _effectiveTracks.size(); }
    TrackId trackIdAt(std::size_t index) const override { return _effectiveTracks.at(index); }
    std::optional<std::size_t> indexOf(TrackId id) const override;

    bool contains(TrackId id) const;

  private:
    void ensureLive() const;
    void loadStoredTracks(library::ListView const& view);
    void rebuildEffectiveTracks();
    std::vector<TrackId> validateStoredRemovals(std::span<ManualStoredRemoveRange const> removals) const;
    void eraseStoredRemovals(std::span<ManualStoredRemoveRange const> removals);
    void publishVisibilityDelta(std::vector<TrackId> const& previousEffective,
                                std::span<TrackId const> updatedTrackIds = {});
    void publishExactMoveDelta(std::vector<TrackId> const& previousEffective, std::span<TrackId const> movedTrackIds);
    void handleParentBatch(TrackSourceDeltaBatch const& batch);

    TrackSourceLease _parentLease;
    IndexedTrackSequence _storedTracks;
    IndexedTrackSequence _effectiveTracks;
    async::Subscription _parentSubscription;
  };
} // namespace ao::rt
