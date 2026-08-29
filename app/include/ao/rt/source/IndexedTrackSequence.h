// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/rt/TrackEditScript.h>

#include <boost/unordered/unordered_flat_map.hpp>

#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <vector>

namespace ao::rt
{
  namespace detail
  {
    class RuntimeOperationProbe;
  }

  class IndexedTrackSequence final
  {
  public:
    IndexedTrackSequence() = default;
    explicit IndexedTrackSequence(std::span<TrackId const> trackIds);

    void assign(std::span<TrackId const> trackIds);
    void clear() noexcept;
    void applyScript(delta::RegularTrackEditScript const& script);

    std::span<TrackId const> ids() const noexcept { return _trackIds; }
    std::vector<TrackId> const& vector() const noexcept { return _trackIds; }
    std::size_t size() const noexcept { return _trackIds.size(); }
    bool empty() const noexcept { return _trackIds.empty(); }
    TrackId at(std::size_t index) const { return _trackIds.at(index); }
    bool contains(TrackId trackId) const { return _indexByTrackId.contains(trackId); }
    std::optional<std::size_t> indexOf(TrackId trackId) const;

  private:
    void replace(std::vector<TrackId> trackIds);
    void updateIndicesFrom(std::size_t start);

    std::vector<TrackId> _trackIds;
    boost::unordered_flat_map<TrackId, std::size_t, std::hash<TrackId>> _indexByTrackId;
    std::size_t _indexRebuildCount = 0;
    std::size_t _incrementalScriptApplicationCount = 0;

    friend class detail::RuntimeOperationProbe;
  };
} // namespace ao::rt
