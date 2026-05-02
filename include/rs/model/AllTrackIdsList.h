// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <rs/model/TrackIdList.h>

#include <rs/library/TrackStore.h>
#include <rs/lmdb/Transaction.h>

#include <flat_set>
#include <vector>

namespace rs::model
{
  /**
   * AllTrackIdsList - Authoritative ordered list of all TrackIds in the library.
   * Loaded from TrackStore and maintained in ascending TrackId order.
   *
   * All notifications are emitted AFTER DB commit to ensure consistency.
   */
  class AllTrackIdsList final : public TrackIdList
  {
  public:
    explicit AllTrackIdsList(rs::library::TrackStore& store);

    using TrackIdList::notifyInserted;
    using TrackIdList::notifyRemoved;
    using TrackIdList::notifyUpdated;

    void reloadFromStore(rs::lmdb::ReadTransaction& txn);
    void notifyInserted(TrackId id);
    void notifyRemoved(TrackId id);
    void clear();

    // TrackIdList interface
    std::size_t size() const override { return _trackIds.size(); }
    TrackId trackIdAt(std::size_t index) const override { return *(_trackIds.begin() + index); }
    std::optional<std::size_t> indexOf(TrackId id) const override;

  private:
    rs::library::TrackStore& _store;
    std::flat_set<TrackId> _trackIds;
  };
} // namespace rs::model