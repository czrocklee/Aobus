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

namespace ao::rt
{
  /**
   * Applies a persisted rank overlay to a filtered parent source.
   *
   * Ranked members appear first in persisted order. Current parent members
   * without a stored rank follow in parent order. Stored IDs outside the
   * current parent membership remain hidden so they recover their rank if they
   * later re-enter.
   */
  class ListOrderSource final : public TrackSource
  {
  public:
    ListOrderSource(std::span<TrackId const> orderTrackIds, TrackSourceLease filteredParentLease);
    ~ListOrderSource() override;

    ListOrderSource(ListOrderSource const&) = delete;
    ListOrderSource& operator=(ListOrderSource const&) = delete;
    ListOrderSource(ListOrderSource&&) = delete;
    ListOrderSource& operator=(ListOrderSource&&) = delete;

    void applyOrderEditScript(delta::RegularTrackEditScript const& script);

    std::span<TrackId const> orderTrackIds() const noexcept { return _orderTrackIds.ids(); }
    TrackSource const& filteredParent() const noexcept { return _filteredParentLease.source(); }

    std::size_t size() const override { return _effectiveTrackIds.size(); }
    TrackId trackIdAt(std::size_t index) const override { return _effectiveTrackIds.at(index); }
    std::optional<std::size_t> indexOf(TrackId id) const override;

    bool contains(TrackId id) const;

  private:
    void discardSnapshot() noexcept override;
    void ensureLive() const;
    void rebuildEffectiveTrackIds();
    void publishVisibilityDelta(std::vector<TrackId> const& previousEffective,
                                std::span<TrackId const> updatedTrackIds = {},
                                std::span<TrackId const> preferredMovedIds = {});
    void handleFilteredParentBatch(TrackSourceDelta const& batch);

    TrackSourceLease _filteredParentLease;
    IndexedTrackSequence _orderTrackIds;
    IndexedTrackSequence _effectiveTrackIds;
    async::Subscription _filteredParentSubscription;
  };
} // namespace ao::rt
