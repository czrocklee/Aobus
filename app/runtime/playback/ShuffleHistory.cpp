// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "runtime/playback/ShuffleHistory.h"

#include <ao/CoreIds.h>
#include <ao/Exception.h>

#include <algorithm>
#include <optional>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

namespace ao::rt
{
  namespace
  {
    bool containsTrack(std::span<TrackId const> const projection, TrackId const trackId) noexcept
    {
      return std::ranges::contains(projection, trackId);
    }

    std::vector<TrackId> eligibleForwardCandidates(std::span<TrackId const> const projection,
                                                   TrackId const currentTrackId,
                                                   bool const currentIsBound,
                                                   bool const repeatAll,
                                                   std::span<TrackId const> const excludedTrackIds)
    {
      auto candidates = std::vector<TrackId>{};
      candidates.reserve(projection.size());

      for (auto const trackId : projection)
      {
        if ((!currentIsBound || trackId != currentTrackId) && !containsTrack(excludedTrackIds, trackId))
        {
          candidates.push_back(trackId);
        }
      }

      if (candidates.empty() && repeatAll && currentIsBound && projection.size() == 1 &&
          projection.front() == currentTrackId && !containsTrack(excludedTrackIds, currentTrackId))
      {
        candidates.push_back(currentTrackId);
      }

      return candidates;
    }
  } // namespace

  ShuffleHistory::ShuffleHistory(CandidateChooser candidateChooser)
    : _candidateChooser{std::move(candidateChooser)}
  {
    if (!_candidateChooser)
    {
      throwException<Exception>("Shuffle history requires a candidate chooser");
    }

    _history.reserve(kHistoryCapacity);
  }

  std::optional<TrackId> ShuffleHistory::forwardCandidate(std::span<TrackId const> const projection,
                                                          TrackId const currentTrackId,
                                                          bool const currentIsBound,
                                                          bool const repeatAll,
                                                          std::span<TrackId const> const excludedTrackIds)
  {
    auto const candidates =
      eligibleForwardCandidates(projection, currentTrackId, currentIsBound, repeatAll, excludedTrackIds);

    if (_optForwardCandidate && containsTrack(candidates, *_optForwardCandidate))
    {
      return _optForwardCandidate;
    }

    _optForwardCandidate.reset();

    if (candidates.empty())
    {
      return std::nullopt;
    }

    auto const selected = _candidateChooser(candidates);

    if (!containsTrack(candidates, selected))
    {
      throwException<Exception>("Shuffle candidate chooser returned an ineligible track");
    }

    _optForwardCandidate = selected;
    return selected;
  }

  void ShuffleHistory::invalidateForwardCandidate() noexcept
  {
    _optForwardCandidate.reset();
  }

  bool ShuffleHistory::discardForwardCandidate(TrackId const candidate) noexcept
  {
    if (!_optForwardCandidate || *_optForwardCandidate != candidate)
    {
      return false;
    }

    _optForwardCandidate.reset();
    return true;
  }

  void ShuffleHistory::recordTransition(TrackId const leavingTrackId,
                                        TrackId const arrivingTrackId,
                                        TransitionOrigin const origin)
  {
    if (leavingTrackId == kInvalidTrackId || arrivingTrackId == kInvalidTrackId)
    {
      throwException<Exception>("Shuffle history requires valid transition track identities");
    }

    if (origin == TransitionOrigin::Restart)
    {
      if (leavingTrackId != arrivingTrackId)
      {
        throwException<Exception>("Playback restart cannot change the current track identity");
      }

      return;
    }

    if (leavingTrackId == arrivingTrackId)
    {
      return;
    }

    invalidateForwardCandidate();

    if (origin == TransitionOrigin::HistoryPrevious)
    {
      return;
    }

    if (_history.size() == kHistoryCapacity)
    {
      _history.erase(_history.begin());
    }

    _history.push_back(leavingTrackId);
  }

  bool ShuffleHistory::hasPrevious(std::span<TrackId const> const projection,
                                   TrackId const currentTrackId) const noexcept
  {
    return std::ranges::any_of(_history | std::views::reverse,
                               [&](TrackId const trackId)
                               { return trackId != currentTrackId && containsTrack(projection, trackId); });
  }

  std::optional<TrackId> ShuffleHistory::popPrevious(std::span<TrackId const> const projection,
                                                     TrackId const currentTrackId)
  {
    while (!_history.empty())
    {
      auto const candidate = _history.back();
      _history.pop_back();

      if (candidate != currentTrackId && containsTrack(projection, candidate))
      {
        return candidate;
      }
    }

    return std::nullopt;
  }

  void ShuffleHistory::clear() noexcept
  {
    invalidateForwardCandidate();
    _history.clear();
  }
} // namespace ao::rt
