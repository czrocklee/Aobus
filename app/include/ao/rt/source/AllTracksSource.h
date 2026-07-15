// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "TrackSource.h"
#include <ao/CoreIds.h>

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace ao::library
{
  class ReadTransaction;
  class TrackStore;
}

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
    explicit AllTracksSource(library::TrackStore const& store);

    using TrackSource::notifyInserted;

    void reloadFromStore(library::ReadTransaction const& transaction);
    void applyCollectionChange(std::span<TrackId const> inserted, std::span<TrackId const> removed);
    void notifyInserted(TrackId id);
    void notifyRemoved(TrackId id);
    void clear();

    // TrackSource interface
    std::size_t size() const override { return _trackIds.size(); }
    TrackId trackIdAt(std::size_t index) const override { return _trackIds.at(index); }
    std::optional<std::size_t> indexOf(TrackId id) const override;

  private:
    library::TrackStore const& _store;
    std::vector<TrackId> _trackIds;
  };
} // namespace ao::rt
