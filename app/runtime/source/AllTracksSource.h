// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/rt/source/TrackSource.h>

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
   * Authoritative ordered source of every TrackId in the library.
   *
   * This implementation detail is owned by TrackSourceCache. Notifications
   * are emitted only after the corresponding database commit.
   */
  class AllTracksSource final : public TrackSource
  {
  public:
    explicit AllTracksSource(library::TrackStore const& store);

    void reloadFromStore(library::ReadTransaction const& transaction);
    void applyCollectionChange(std::span<TrackId const> inserted, std::span<TrackId const> removed);
    void applyMetadataChange(std::span<TrackId const> trackIds);

    std::size_t size() const override { return _trackIds.size(); }
    TrackId trackIdAt(std::size_t index) const override { return _trackIds.at(index); }
    std::optional<std::size_t> indexOf(TrackId id) const override;

  private:
    void discardSnapshot() noexcept override;

    library::TrackStore const& _store;
    std::vector<TrackId> _trackIds;
  };
} // namespace ao::rt
