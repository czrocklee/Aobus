// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include "core/model/TrackIdList.h"

#include <rs/core/TrackStore.h>
#include <rs/lmdb/Transaction.h>

#include <flat_set>
#include <vector>

namespace app::core::model
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
    explicit AllTrackIdsList(rs::core::TrackStore& store);

    void reloadFromStore(rs::lmdb::ReadTransaction& txn);
    void notifyInserted(TrackId id);
    void notifyUpdated(TrackId id);
    void notifyRemoved(TrackId id);
    void clear();

    // TrackIdList interface
    std::size_t size() const override { return _trackIds.size(); }
    TrackId trackIdAt(std::size_t index) const override { return *(_trackIds.begin() + index); }
    std::optional<std::size_t> indexOf(TrackId id) const override;

  private:
    rs::core::TrackStore* _store;
    std::flat_set<TrackId> _trackIds;
  };

} // namespace app::core::model