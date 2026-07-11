// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/CoreIds.h>
#include <ao/library/ListView.h>
#include <ao/rt/TrackEditScript.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/source/ManualListSource.h>
#include <ao/rt/source/TrackSource.h>
#include <ao/rt/source/TrackSourceDelta.h>
#include <ao/rt/source/TrackSourceEditScript.h>
#include <ao/rt/source/TrackSourceLease.h>

#include <boost/unordered/unordered_flat_set.hpp>
#include <gsl-lite/gsl-lite.hpp>

#include <algorithm>
#include <cstddef>
#include <functional>
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

    auto const previousEffective = _effectiveTracks.vector();
    loadStoredTracks(view);
    rebuildEffectiveTracks();

    if (previousEffective == _effectiveTracks.vector())
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

    gsl_Assert(operation.storedIndex <= _storedTracks.size());

    for (auto const trackId : operation.trackIds)
    {
      gsl_Assert(!_storedTracks.contains(trackId) && std::ranges::count(operation.trackIds, trackId) == 1);
    }

    auto const previousEffective = _effectiveTracks.vector();
    _storedTracks.applyScript(delta::RegularTrackEditScript{
      .edits = {delta::InsertRange{.start = operation.storedIndex, .trackIds = operation.trackIds}}});
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
    auto const previousEffective = _effectiveTracks.vector();
    eraseStoredRemovals(operation.removals);
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

    gsl_Assert(removedInStoredOrder == operation.insertedTrackIds);

    auto const remainingSize = _storedTracks.size() - removedInStoredOrder.size();

    gsl_Assert(operation.insertionIndexAfterRemoval <= remainingSize);

    auto const previousEffective = _effectiveTracks.vector();
    auto script = delta::RegularTrackEditScript{};

    for (auto const& removal : operation.removals)
    {
      script.edits.emplace_back(delta::RemoveRange{.start = removal.start, .trackIds = removal.trackIds});
    }

    script.edits.emplace_back(
      delta::InsertRange{.start = operation.insertionIndexAfterRemoval, .trackIds = operation.insertedTrackIds});
    _storedTracks.applyScript(script);
    rebuildEffectiveTracks();

    if (previousEffective == _effectiveTracks.vector())
    {
      return;
    }

    publishExactMoveDelta(previousEffective, operation.insertedTrackIds);
  }

  bool ManualListSource::contains(TrackId const id) const
  {
    return _effectiveTracks.contains(id);
  }

  std::optional<std::size_t> ManualListSource::indexOf(TrackId const id) const
  {
    return _effectiveTracks.indexOf(id);
  }

  void ManualListSource::ensureLive() const
  {
    gsl_Assert(state() != TrackSourceState::Invalidated);
  }

  void ManualListSource::loadStoredTracks(library::ListView const& view)
  {
    auto const tracks = view.tracks();
    _storedTracks.assign(std::span<TrackId const>{tracks.begin(), tracks.size()});
  }

  void ManualListSource::rebuildEffectiveTracks()
  {
    auto effective = std::vector<TrackId>{};
    effective.reserve(_storedTracks.size());

    for (auto const trackId : _storedTracks.ids())
    {
      if (_parentLease->indexOf(trackId))
      {
        effective.push_back(trackId);
      }
    }

    _effectiveTracks.assign(effective);
  }

  std::vector<TrackId> ManualListSource::validateStoredRemovals(
    std::span<ManualStoredRemoveRange const> const removals) const
  {
    gsl_Assert(!removals.empty());

    auto selectedIds = boost::unordered_flat_set<TrackId, std::hash<TrackId>>{};
    auto upperBound = _storedTracks.size();

    for (auto const& removal : removals)
    {
      gsl_Assert(!removal.trackIds.empty() && removal.start <= upperBound &&
                 removal.trackIds.size() <= upperBound - removal.start);

      for (std::size_t offset = 0; offset < removal.trackIds.size(); ++offset)
      {
        auto const trackId = removal.trackIds[offset];

        gsl_Assert(_storedTracks.at(removal.start + offset) == trackId);

        auto const inserted = selectedIds.emplace(trackId).second;
        gsl_Assert(inserted);
      }

      upperBound = removal.start;
    }

    auto removedInStoredOrder = std::vector<TrackId>{};
    removedInStoredOrder.reserve(selectedIds.size());

    for (auto const trackId : _storedTracks.ids())
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
    auto script = delta::RegularTrackEditScript{};

    for (auto const& removal : removals)
    {
      script.edits.emplace_back(delta::RemoveRange{.start = removal.start, .trackIds = removal.trackIds});
    }

    _storedTracks.applyScript(script);
  }

  void ManualListSource::publishVisibilityDelta(std::vector<TrackId> const& previousEffective,
                                                std::span<TrackId const> const updatedTrackIds)
  {
    auto batch = sourceBatchOf(delta::diff(previousEffective, _effectiveTracks.ids(), updatedTrackIds));

    if (batch.deltas.empty())
    {
      return;
    }

    std::ignore = publishDeltaBatch(std::move(batch), previousEffective.size());
  }

  void ManualListSource::publishExactMoveDelta(std::vector<TrackId> const& previousEffective,
                                               std::span<TrackId const> const movedTrackIds)
  {
    auto batch = sourceBatchOf(delta::diff(previousEffective, _effectiveTracks.ids(), {}, movedTrackIds));

    if (!batch.deltas.empty())
    {
      std::ignore = publishDeltaBatch(std::move(batch), previousEffective.size());
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

    auto const previousEffective = _effectiveTracks.vector();
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
