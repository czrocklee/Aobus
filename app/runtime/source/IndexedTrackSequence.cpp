// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "runtime/source/IndexedTrackSequence.h"

#include "runtime/RuntimeOperationProbe.h"
#include <ao/Contract.h>
#include <ao/CoreIds.h>
#include <ao/rt/TrackEditScript.h>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <span>
#include <utility>
#include <variant>
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
    AO_INVARIANT(delta::validate(script, _trackIds.size()));

    if (script.edits.empty())
    {
      return;
    }

    std::size_t structuralEditCount = 0;

    for (auto const& edit : script.edits)
    {
      if (!std::holds_alternative<delta::UpdateRange>(edit))
      {
        ++structuralEditCount;
      }
    }

    // Two structural ranges cover the common remove-plus-insert move. Larger
    // scripts use the linear bulk builder instead of repeatedly shifting vector storage.
    constexpr std::size_t kMaximumInPlaceStructuralEdits = 2;

    if (structuralEditCount > kMaximumInPlaceStructuralEdits)
    {
      auto appliedRes = delta::apply(_trackIds, script);
      AO_INVARIANT(appliedRes);
      replace(std::move(*appliedRes));
      return;
    }

    auto optFirstChangedIndex = std::optional<std::size_t>{};
    auto recordChangedIndex = [&optFirstChangedIndex](std::size_t const index)
    {
      if (!optFirstChangedIndex || index < *optFirstChangedIndex)
      {
        optFirstChangedIndex = index;
      }
    };

    for (auto const& edit : script.edits)
    {
      auto const* const removal = std::get_if<delta::RemoveRange>(&edit);

      if (removal == nullptr)
      {
        break;
      }

      auto const existing = std::span<TrackId const>{_trackIds}.subspan(removal->start, removal->trackIds.size());
      AO_INVARIANT(std::ranges::equal(existing, removal->trackIds));

      for (auto const trackId : removal->trackIds)
      {
        auto const erased = _indexByTrackId.erase(trackId);
        AO_INVARIANT(erased == 1);
      }

      auto const first = _trackIds.begin() + static_cast<std::ptrdiff_t>(removal->start);
      _trackIds.erase(first, first + static_cast<std::ptrdiff_t>(removal->trackIds.size()));
      recordChangedIndex(removal->start);
    }

    for (auto const& edit : script.edits)
    {
      auto const* const insertion = std::get_if<delta::InsertRange>(&edit);

      if (insertion == nullptr)
      {
        continue;
      }

      auto const position = _trackIds.begin() + static_cast<std::ptrdiff_t>(insertion->start);
      _trackIds.insert(position, insertion->trackIds.begin(), insertion->trackIds.end());

      for (std::size_t offset = 0; offset < insertion->trackIds.size(); ++offset)
      {
        auto const inserted = _indexByTrackId.emplace(insertion->trackIds[offset], insertion->start + offset).second;
        AO_INVARIANT(inserted);
      }

      recordChangedIndex(insertion->start);
    }

    if (optFirstChangedIndex)
    {
      updateIndicesFrom(*optFirstChangedIndex);
    }

    for (auto const& edit : script.edits)
    {
      auto const* const update = std::get_if<delta::UpdateRange>(&edit);

      if (update == nullptr)
      {
        continue;
      }

      auto const existing = std::span<TrackId const>{_trackIds}.subspan(update->start, update->trackIds.size());
      AO_INVARIANT(std::ranges::equal(existing, update->trackIds));
    }

    ++_incrementalScriptApplicationCount;
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
      AO_INVARIANT(inserted);
    }

    _trackIds = std::move(trackIds);
    _indexByTrackId = std::move(indexByTrackId);
    ++_indexRebuildCount;
  }

  void IndexedTrackSequence::updateIndicesFrom(std::size_t const start)
  {
    for (auto index = start; index < _trackIds.size(); ++index)
    {
      auto const it = _indexByTrackId.find(_trackIds[index]);
      AO_INVARIANT(it != _indexByTrackId.end());
      it->second = index;
    }
  }

  namespace detail
  {
    IndexedTrackSequenceOperationCounts RuntimeOperationProbe::counts(IndexedTrackSequence const& sequence) noexcept
    {
      return {.indexRebuilds = sequence._indexRebuildCount,
              .incrementalScriptApplications = sequence._incrementalScriptApplicationCount};
    }
  } // namespace detail
} // namespace ao::rt
