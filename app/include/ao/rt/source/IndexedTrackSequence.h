// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/rt/TrackEditScript.h>

#include <boost/unordered/unordered_flat_map.hpp>

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace ao::rt
{
  struct IndexedTrackSequenceOperationCounts final
  {
    std::size_t indexRebuilds = 0;
  };

  class IndexedTrackSequence final
  {
  public:
    IndexedTrackSequence() = default;
    explicit IndexedTrackSequence(std::span<TrackId const> trackIds);

    void assign(std::span<TrackId const> trackIds);
    void clear() noexcept;
    void applyScript(delta::RegularTrackEditScript const& script);

    // Rare point operations are O(n), including one complete index rebuild.
    void insert(std::size_t index, TrackId trackId);
    void remove(std::size_t index, TrackId trackId);

    std::span<TrackId const> ids() const noexcept { return _trackIds; }
    std::vector<TrackId> const& vector() const noexcept { return _trackIds; }
    std::size_t size() const noexcept { return _trackIds.size(); }
    bool empty() const noexcept { return _trackIds.empty(); }
    TrackId at(std::size_t index) const { return _trackIds.at(index); }
    bool contains(TrackId trackId) const { return _indexByTrackId.contains(trackId); }
    std::optional<std::size_t> indexOf(TrackId trackId) const;
    IndexedTrackSequenceOperationCounts operationCounts() const noexcept { return _operationCounts; }

  private:
    void replace(std::vector<TrackId> trackIds);

    std::vector<TrackId> _trackIds;
    boost::unordered_flat_map<TrackId, std::size_t, std::hash<TrackId>> _indexByTrackId;
    IndexedTrackSequenceOperationCounts _operationCounts;
  };
} // namespace ao::rt
