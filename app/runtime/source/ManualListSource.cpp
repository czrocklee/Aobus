// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/CoreIds.h>
#include <ao/library/ListView.h>
#include <ao/rt/TrackEditScript.h>
#include <ao/rt/source/ManualListSource.h>
#include <ao/rt/source/TrackSource.h>
#include <ao/rt/source/TrackSourceDelta.h>
#include <ao/rt/source/TrackSourceLease.h>

#include <boost/unordered/unordered_flat_set.hpp>
#include <gsl-lite/gsl-lite.hpp>

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
      _parentLease->subscribe([this](TrackSourceDelta const& batch) noexcept { handleParentBatch(batch); });
  }

  ManualListSource::~ManualListSource()
  {
    _parentSubscription.reset();
  }

  void ManualListSource::applyManualEditScript(delta::RegularTrackEditScript const& script)
  {
    ensureLive();
    gsl_Assert(!script.edits.empty() && delta::validate(script, _storedTracks.size()));

    auto const previousEffective = _effectiveTracks.vector();
    auto removedTrackIds = boost::unordered_flat_set<TrackId, std::hash<TrackId>>{};
    auto preferredMovedIds = std::vector<TrackId>{};

    for (auto const& edit : script.edits)
    {
      if (auto const* removal = std::get_if<delta::RemoveRange>(&edit); removal != nullptr)
      {
        removedTrackIds.insert(removal->trackIds.begin(), removal->trackIds.end());
      }
      else if (auto const* insertion = std::get_if<delta::InsertRange>(&edit); insertion != nullptr)
      {
        for (auto const trackId : insertion->trackIds)
        {
          if (removedTrackIds.contains(trackId))
          {
            preferredMovedIds.push_back(trackId);
          }
        }
      }
    }

    _storedTracks.applyScript(script);
    rebuildEffectiveTracks();
    publishVisibilityDelta(previousEffective, {}, preferredMovedIds);
  }

  bool ManualListSource::contains(TrackId const id) const
  {
    return _effectiveTracks.contains(id);
  }

  std::optional<std::size_t> ManualListSource::indexOf(TrackId const id) const
  {
    return _effectiveTracks.indexOf(id);
  }

  void ManualListSource::discardSnapshot() noexcept
  {
    _parentSubscription.reset();
    _storedTracks.clear();
    _effectiveTracks.clear();
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

  void ManualListSource::publishVisibilityDelta(std::vector<TrackId> const& previousEffective,
                                                std::span<TrackId const> const updatedTrackIds,
                                                std::span<TrackId const> const preferredMovedIds)
  {
    auto script = delta::diff(previousEffective, _effectiveTracks.ids(), updatedTrackIds, preferredMovedIds);

    if (script.edits.empty())
    {
      return;
    }

    std::ignore = publishDelta(std::move(script), previousEffective.size());
  }

  void ManualListSource::handleParentBatch(TrackSourceDelta const& batch)
  {
    if (state() == TrackSourceState::Invalidated)
    {
      return;
    }

    if (std::holds_alternative<SourceInvalidated>(batch))
    {
      _parentSubscription.reset();
      invalidate();
      return;
    }

    auto const previousEffective = _effectiveTracks.vector();
    rebuildEffectiveTracks();

    if (std::holds_alternative<SourceReset>(batch))
    {
      std::ignore = publishDelta(SourceReset{}, previousEffective.size());
      return;
    }

    auto updatedTrackIds = std::vector<TrackId>{};

    for (auto const& edit : std::get<delta::RegularTrackEditScript>(batch).edits)
    {
      if (auto const* update = std::get_if<delta::UpdateRange>(&edit); update != nullptr)
      {
        updatedTrackIds.append_range(update->trackIds);
      }
    }

    publishVisibilityDelta(previousEffective, updatedTrackIds);
  }
} // namespace ao::rt
