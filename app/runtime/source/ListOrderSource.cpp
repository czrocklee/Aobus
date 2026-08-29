// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "runtime/source/ListOrderSource.h"

#include <ao/Contract.h>
#include <ao/CoreIds.h>
#include <ao/rt/TrackEditScript.h>
#include <ao/rt/source/TrackSource.h>
#include <ao/rt/source/TrackSourceDelta.h>
#include <ao/rt/source/TrackSourceLease.h>

#include <boost/unordered/unordered_flat_set.hpp>

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
  ListOrderSource::ListOrderSource(std::span<TrackId const> const orderTrackIds, TrackSourceLease filteredParentLease)
    : _filteredParentLease{std::move(filteredParentLease)}, _orderTrackIds{orderTrackIds}
  {
    rebuildEffectiveTrackIds();
    _filteredParentSubscription =
      _filteredParentLease->subscribe([this](TrackSourceDelta const& batch) { handleFilteredParentBatch(batch); });
  }

  ListOrderSource::~ListOrderSource()
  {
    _filteredParentSubscription.reset();
  }

  void ListOrderSource::applyOrderEditScript(delta::RegularTrackEditScript const& script)
  {
    ensureLive();
    AO_INVARIANT(!script.edits.empty() && delta::validate(script, _orderTrackIds.size()));

    auto const previousEffective = _effectiveTrackIds.vector();
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

    _orderTrackIds.applyScript(script);
    rebuildEffectiveTrackIds();
    publishVisibilityDelta(previousEffective, {}, preferredMovedIds);
  }

  bool ListOrderSource::contains(TrackId const id) const
  {
    return _effectiveTrackIds.contains(id);
  }

  std::optional<std::size_t> ListOrderSource::indexOf(TrackId const id) const
  {
    return _effectiveTrackIds.indexOf(id);
  }

  void ListOrderSource::discardSnapshot() noexcept
  {
    _filteredParentSubscription.reset();
    _orderTrackIds.clear();
    _effectiveTrackIds.clear();
  }

  void ListOrderSource::ensureLive() const
  {
    AO_INVARIANT(state() != TrackSourceState::Invalidated);
  }

  void ListOrderSource::rebuildEffectiveTrackIds()
  {
    auto effective = std::vector<TrackId>{};
    effective.reserve(_orderTrackIds.size() + _filteredParentLease->size());

    for (auto const trackId : _orderTrackIds.ids())
    {
      if (_filteredParentLease->indexOf(trackId))
      {
        effective.push_back(trackId);
      }
    }

    for (std::size_t index = 0; index < _filteredParentLease->size(); ++index)
    {
      if (auto const trackId = _filteredParentLease->trackIdAt(index); !_orderTrackIds.contains(trackId))
      {
        effective.push_back(trackId);
      }
    }

    _effectiveTrackIds.assign(effective);
  }

  void ListOrderSource::publishVisibilityDelta(std::vector<TrackId> const& previousEffective,
                                               std::span<TrackId const> const updatedTrackIds,
                                               std::span<TrackId const> const preferredMovedIds)
  {
    auto script = delta::diff(previousEffective, _effectiveTrackIds.ids(), updatedTrackIds, preferredMovedIds);

    if (script.edits.empty())
    {
      return;
    }

    std::ignore = publishDelta(std::move(script), previousEffective.size());
  }

  void ListOrderSource::handleFilteredParentBatch(TrackSourceDelta const& batch)
  {
    if (state() == TrackSourceState::Invalidated)
    {
      return;
    }

    if (std::holds_alternative<SourceInvalidated>(batch))
    {
      _filteredParentSubscription.reset();
      invalidate();
      return;
    }

    if (std::holds_alternative<SourceReset>(batch))
    {
      auto const previousEffective = _effectiveTrackIds.vector();
      rebuildEffectiveTrackIds();
      std::ignore = publishDelta(SourceReset{}, previousEffective.size());
      return;
    }

    auto const& parentScript = std::get<delta::RegularTrackEditScript>(batch);

    if (_orderTrackIds.empty())
    {
      auto const previousSize = _effectiveTrackIds.size();
      _effectiveTrackIds.applyScript(parentScript);
      std::ignore = publishDelta(parentScript, previousSize);
      return;
    }

    auto const previousEffective = _effectiveTrackIds.vector();
    rebuildEffectiveTrackIds();
    auto updatedTrackIds = std::vector<TrackId>{};

    for (auto const& edit : parentScript.edits)
    {
      if (auto const* update = std::get_if<delta::UpdateRange>(&edit); update != nullptr)
      {
        updatedTrackIds.append_range(update->trackIds);
      }
    }

    publishVisibilityDelta(previousEffective, updatedTrackIds);
  }
} // namespace ao::rt
