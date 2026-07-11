// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "runtime/source/TrackSourceDeltaBuilder.h"
#include <ao/CoreIds.h>
#include <ao/Exception.h>
#include <ao/library/ListView.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/source/ManualListSource.h>
#include <ao/rt/source/TrackSource.h>
#include <ao/rt/source/TrackSourceDelta.h>
#include <ao/rt/source/TrackSourceLease.h>

#include <cstddef>
#include <optional>
#include <span>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace ao::rt
{
  ManualListSource::ManualListSource(library::ListView const& view, TrackSourceLease parentLease)
    : _parentLease{std::move(parentLease)}
  {
    loadStoredTracks(view);
    rebuildEffectiveTracks();
    _parentSubscription =
      _parentLease->subscribe([this](TrackSourceDeltaBatch const& batch) { handleParentBatch(batch); });
  }

  ManualListSource::~ManualListSource()
  {
    _parentSubscription.reset();
  }

  void ManualListSource::reloadFromListView(library::ListView const& view)
  {
    ensureLive();

    auto const previousEffective = _effectiveTrackIds;
    loadStoredTracks(view);
    rebuildEffectiveTracks();

    if (previousEffective == _effectiveTrackIds)
    {
      return;
    }

    std::ignore = publishDeltaBatch(TrackSourceDeltaBatch{.deltas = {SourceReset{}}}, previousEffective.size());
  }

  void ManualListSource::applyManualTracksInsert(ManualTracksInsert const& operation)
  {
    ensureLive();

    if (operation.trackIds.empty())
    {
      return;
    }

    if (operation.storedIndex > _storedTrackIds.size())
    {
      throwException<Exception>("Manual track insertion index is outside the stored order");
    }

    auto insertedIds = TrackIndexMap{};
    insertedIds.reserve(operation.trackIds.size());

    for (auto const trackId : operation.trackIds)
    {
      if (_storedIndexByTrackId.contains(trackId) || !insertedIds.emplace(trackId, insertedIds.size()).second)
      {
        throwException<Exception>("Manual track insertion would duplicate a stored track");
      }
    }

    auto const previousEffective = _effectiveTrackIds;
    _storedTrackIds.insert(_storedTrackIds.begin() + static_cast<std::ptrdiff_t>(operation.storedIndex),
                           operation.trackIds.begin(),
                           operation.trackIds.end());
    rebuildStoredIndex();
    rebuildEffectiveTracks();
    publishVisibilityDelta(previousEffective);
  }

  void ManualListSource::applyManualTracksRemove(ManualTracksRemove const& operation)
  {
    ensureLive();

    if (operation.removals.empty())
    {
      return;
    }

    std::ignore = validateStoredRemovals(operation.removals);
    auto const previousEffective = _effectiveTrackIds;
    eraseStoredRemovals(operation.removals);
    rebuildStoredIndex();
    rebuildEffectiveTracks();
    publishVisibilityDelta(previousEffective);
  }

  void ManualListSource::applyManualTracksMove(ManualTracksMove const& operation)
  {
    ensureLive();

    if (operation.removals.empty() && operation.insertedTrackIds.empty())
    {
      return;
    }

    auto const removedInStoredOrder = validateStoredRemovals(operation.removals);

    if (removedInStoredOrder != operation.insertedTrackIds)
    {
      throwException<Exception>("Manual track move identities do not match the stored removals");
    }

    auto const remainingSize = _storedTrackIds.size() - removedInStoredOrder.size();

    if (operation.insertionIndexAfterRemoval > remainingSize)
    {
      throwException<Exception>("Manual track move destination is outside the post-removal order");
    }

    auto const previousEffective = _effectiveTrackIds;
    eraseStoredRemovals(operation.removals);
    _storedTrackIds.insert(_storedTrackIds.begin() + static_cast<std::ptrdiff_t>(operation.insertionIndexAfterRemoval),
                           operation.insertedTrackIds.begin(),
                           operation.insertedTrackIds.end());
    rebuildStoredIndex();
    rebuildEffectiveTracks();

    if (previousEffective == _effectiveTrackIds)
    {
      return;
    }

    publishExactMoveDelta(previousEffective, operation.insertedTrackIds);
  }

  bool ManualListSource::contains(TrackId const id) const
  {
    return _effectiveIndexByTrackId.contains(id);
  }

  std::optional<std::size_t> ManualListSource::indexOf(TrackId const id) const
  {
    if (auto const it = _effectiveIndexByTrackId.find(id); it != _effectiveIndexByTrackId.end())
    {
      return it->second;
    }

    return std::nullopt;
  }

  void ManualListSource::ensureLive() const
  {
    if (state() == TrackSourceState::Invalidated)
    {
      throwException<Exception>("Cannot mutate an invalidated manual list source");
    }
  }

  void ManualListSource::loadStoredTracks(library::ListView const& view)
  {
    _storedTrackIds.clear();
    _storedIndexByTrackId.clear();
    _storedTrackIds.reserve(view.tracks().size());
    _storedIndexByTrackId.reserve(view.tracks().size());

    for (auto const trackId : view.tracks())
    {
      if (_storedIndexByTrackId.emplace(trackId, _storedTrackIds.size()).second)
      {
        _storedTrackIds.push_back(trackId);
      }
    }
  }

  void ManualListSource::rebuildStoredIndex()
  {
    _storedIndexByTrackId.clear();
    _storedIndexByTrackId.reserve(_storedTrackIds.size());

    for (std::size_t index = 0; index < _storedTrackIds.size(); ++index)
    {
      if (!_storedIndexByTrackId.emplace(_storedTrackIds[index], index).second)
      {
        throwException<Exception>("Manual stored track order contains a duplicate identity");
      }
    }
  }

  void ManualListSource::rebuildEffectiveTracks()
  {
    _effectiveTrackIds.clear();
    _effectiveIndexByTrackId.clear();
    _effectiveTrackIds.reserve(_storedTrackIds.size());
    _effectiveIndexByTrackId.reserve(_storedTrackIds.size());

    for (auto const trackId : _storedTrackIds)
    {
      if (_parentLease->indexOf(trackId))
      {
        _effectiveIndexByTrackId.emplace(trackId, _effectiveTrackIds.size());
        _effectiveTrackIds.push_back(trackId);
      }
    }
  }

  std::vector<TrackId> ManualListSource::validateStoredRemovals(
    std::span<ManualStoredRemoveRange const> const removals) const
  {
    if (removals.empty())
    {
      throwException<Exception>("Manual stored removal operation requires at least one range");
    }

    auto selectedIds = TrackIndexMap{};
    auto upperBound = _storedTrackIds.size();
    std::size_t selectedCount = 0;

    for (auto const& removal : removals)
    {
      if (removal.trackIds.empty() || removal.start > upperBound ||
          removal.trackIds.size() > upperBound - removal.start)
      {
        throwException<Exception>("Manual stored removal ranges must be non-overlapping and descending");
      }

      for (std::size_t offset = 0; offset < removal.trackIds.size(); ++offset)
      {
        auto const trackId = removal.trackIds[offset];

        if (_storedTrackIds[removal.start + offset] != trackId)
        {
          throwException<Exception>("Manual stored removal identities do not match the stored order");
        }

        if (!selectedIds.emplace(trackId, selectedCount++).second)
        {
          throwException<Exception>("Manual stored removal identities overlap");
        }
      }

      upperBound = removal.start;
    }

    auto removedInStoredOrder = std::vector<TrackId>{};
    removedInStoredOrder.reserve(selectedIds.size());

    for (auto const trackId : _storedTrackIds)
    {
      if (selectedIds.contains(trackId))
      {
        removedInStoredOrder.push_back(trackId);
      }
    }

    return removedInStoredOrder;
  }

  void ManualListSource::eraseStoredRemovals(std::span<ManualStoredRemoveRange const> const removals)
  {
    for (auto const& removal : removals)
    {
      auto const first = _storedTrackIds.begin() + static_cast<std::ptrdiff_t>(removal.start);
      _storedTrackIds.erase(first, first + static_cast<std::ptrdiff_t>(removal.trackIds.size()));
    }
  }

  void ManualListSource::publishVisibilityDelta(std::vector<TrackId> const& previousEffective,
                                                std::span<TrackId const> const updatedTrackIds)
  {
    auto previousIndexByTrackId = TrackIndexMap{};
    previousIndexByTrackId.reserve(previousEffective.size());

    for (std::size_t index = 0; index < previousEffective.size(); ++index)
    {
      previousIndexByTrackId.emplace(previousEffective[index], index);
    }

    auto builder = TrackSourceDeltaBuilder{previousEffective.size()};

    for (std::size_t index = 0; index < previousEffective.size(); ++index)
    {
      if (auto const trackId = previousEffective[index]; !_effectiveIndexByTrackId.contains(trackId))
      {
        builder.remove(index, trackId);
      }
    }

    for (std::size_t index = 0; index < _effectiveTrackIds.size(); ++index)
    {
      if (auto const trackId = _effectiveTrackIds[index]; !previousIndexByTrackId.contains(trackId))
      {
        builder.insert(index, trackId);
      }
    }

    auto optBatch = builder.build();
    auto batch = optBatch ? std::move(*optBatch) : TrackSourceDeltaBatch{};

    if (!updatedTrackIds.empty())
    {
      auto updatedIds = TrackIndexMap{};
      updatedIds.reserve(updatedTrackIds.size());

      for (auto const trackId : updatedTrackIds)
      {
        updatedIds.emplace(trackId, updatedIds.size());
      }

      for (std::size_t index = 0; index < _effectiveTrackIds.size(); ++index)
      {
        auto const trackId = _effectiveTrackIds[index];

        if (!updatedIds.contains(trackId) || !previousIndexByTrackId.contains(trackId))
        {
          continue;
        }

        if (!batch.deltas.empty() && std::holds_alternative<SourceUpdateRange>(batch.deltas.back()))
        {
          auto& previousRange = std::get<SourceUpdateRange>(batch.deltas.back());

          if (previousRange.start + previousRange.trackIds.size() == index)
          {
            previousRange.trackIds.push_back(trackId);
            continue;
          }
        }

        batch.deltas.push_back(SourceUpdateRange{.start = index, .trackIds = {trackId}});
      }
    }

    if (batch.deltas.empty())
    {
      return;
    }

    std::ignore = publishDeltaBatch(std::move(batch), previousEffective.size());
  }

  void ManualListSource::publishExactMoveDelta(std::vector<TrackId> const& previousEffective,
                                               std::span<TrackId const> const movedTrackIds)
  {
    auto previousIndexByTrackId = TrackIndexMap{};
    previousIndexByTrackId.reserve(previousEffective.size());

    for (std::size_t index = 0; index < previousEffective.size(); ++index)
    {
      previousIndexByTrackId.emplace(previousEffective[index], index);
    }

    auto builder = TrackSourceDeltaBuilder{previousEffective.size()};

    for (auto const trackId : movedTrackIds)
    {
      auto const previous = previousIndexByTrackId.find(trackId);

      if (auto const current = _effectiveIndexByTrackId.find(trackId);
          previous != previousIndexByTrackId.end() && current != _effectiveIndexByTrackId.end())
      {
        builder.remove(previous->second, trackId);
        builder.insert(current->second, trackId);
      }
    }

    if (auto optBatch = builder.build(); optBatch)
    {
      std::ignore = publishDeltaBatch(std::move(*optBatch), previousEffective.size());
    }
  }

  void ManualListSource::handleParentBatch(TrackSourceDeltaBatch const& batch)
  {
    if (state() == TrackSourceState::Invalidated)
    {
      return;
    }

    if (batch.deltas.size() == 1 && std::holds_alternative<SourceInvalidated>(batch.deltas.front()))
    {
      _parentSubscription.reset();
      invalidate();
      return;
    }

    auto const previousEffective = _effectiveTrackIds;
    rebuildEffectiveTracks();

    if (batch.deltas.size() == 1 && std::holds_alternative<SourceReset>(batch.deltas.front()))
    {
      std::ignore = publishDeltaBatch(TrackSourceDeltaBatch{.deltas = {SourceReset{}}}, previousEffective.size());
      return;
    }

    auto updatedTrackIds = std::vector<TrackId>{};

    for (auto const& delta : batch.deltas)
    {
      if (auto const* update = std::get_if<SourceUpdateRange>(&delta); update != nullptr)
      {
        updatedTrackIds.append_range(update->trackIds);
      }
    }

    publishVisibilityDelta(previousEffective, updatedTrackIds);
  }
} // namespace ao::rt
