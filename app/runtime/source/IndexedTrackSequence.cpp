// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/CoreIds.h>
#include <ao/rt/TrackEditScript.h>
#include <ao/rt/source/IndexedTrackSequence.h>

#include <gsl-lite/gsl-lite.hpp>

#include <cstddef>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace ao::rt
{
  IndexedTrackSequence::IndexedTrackSequence(std::span<TrackId const> const trackIds)
  {
    assign(trackIds);
  }

  void IndexedTrackSequence::assign(std::span<TrackId const> const trackIds)
  {
    replace(std::vector<TrackId>{trackIds.begin(), trackIds.end()});
  }

  void IndexedTrackSequence::clear() noexcept
  {
    _trackIds.clear();
    _indexByTrackId.clear();
  }

  void IndexedTrackSequence::applyScript(delta::RegularTrackEditScript const& script)
  {
    auto result = delta::apply(_trackIds, script);
    gsl_Assert(result);
    replace(std::move(*result));
  }

  void IndexedTrackSequence::insert(std::size_t const index, TrackId const trackId)
  {
    applyScript(delta::RegularTrackEditScript{.edits = {delta::InsertRange{.start = index, .trackIds = {trackId}}}});
  }

  void IndexedTrackSequence::remove(std::size_t const index, TrackId const trackId)
  {
    applyScript(delta::RegularTrackEditScript{.edits = {delta::RemoveRange{.start = index, .trackIds = {trackId}}}});
  }

  std::optional<std::size_t> IndexedTrackSequence::indexOf(TrackId const trackId) const
  {
    if (auto const it = _indexByTrackId.find(trackId); it != _indexByTrackId.end())
    {
      return it->second;
    }

    return std::nullopt;
  }

  void IndexedTrackSequence::replace(std::vector<TrackId> trackIds)
  {
    auto indexByTrackId = decltype(_indexByTrackId){};
    indexByTrackId.reserve(trackIds.size());

    for (std::size_t index = 0; index < trackIds.size(); ++index)
    {
      auto const inserted = indexByTrackId.emplace(trackIds[index], index).second;
      gsl_Assert(inserted);
    }

    _trackIds = std::move(trackIds);
    _indexByTrackId = std::move(indexByTrackId);
    ++_operationCounts.indexRebuilds;
  }
} // namespace ao::rt
