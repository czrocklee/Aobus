// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "TrackSource.h"

#include <ao/library/TrackStore.h>
#include <ao/lmdb/Transaction.h>

#include <flat_set>
#include <vector>

namespace ao::rt
{
  /**
   * AllTracksSource - Authoritative ordered list of all TrackIds in the library.
   * Loaded from TrackStore and maintained in ascending TrackId order.
   *
   * All notifications are emitted AFTER DB commit to ensure consistency.
   */
  class AllTracksSource final : public TrackSource
  {
  public:
    explicit AllTracksSource(ao::library::TrackStore& store);

    using TrackSource::notifyInserted;
    using TrackSource::notifyRemoved;
    using TrackSource::notifyUpdated;

    void reloadFromStore(ao::lmdb::ReadTransaction const& txn);
    void notifyInserted(TrackId id);
    void notifyRemoved(TrackId id);
    void clear();

    // TrackSource interface
    std::size_t size() const override { return _trackIds.size(); }
    TrackId trackIdAt(std::size_t index) const override { return *(_trackIds.begin() + index); }
    std::optional<std::size_t> indexOf(TrackId id) const override;

  private:
    ao::library::TrackStore& _store;
    std::flat_set<TrackId> _trackIds;
  };
}
