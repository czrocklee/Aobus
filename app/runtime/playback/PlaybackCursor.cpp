// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "runtime/playback/PlaybackCursor.h"

#include "runtime/playback/ProjectionAnchor.h"
#include <ao/CoreIds.h>
#include <ao/Exception.h>
#include <ao/rt/PlaybackLaunchSpec.h>
#include <ao/rt/PlaybackMode.h>
#include <ao/rt/projection/TrackListProjection.h>

#include <cstddef>
#include <optional>
#include <utility>

namespace ao::rt
{
  namespace
  {
    PlaybackCursor::CommandResolution startTrack(TrackId const trackId)
    {
      return PlaybackCursor::CommandResolution{
        .action = PlaybackCursor::CommandAction::StartTrack,
        .trackId = trackId,
      };
    }

    PlaybackCursor::CommandResolution restartCurrent(TrackId const trackId)
    {
      return PlaybackCursor::CommandResolution{
        .action = PlaybackCursor::CommandAction::RestartCurrent,
        .trackId = trackId,
      };
    }

    PlaybackCursor::CommandResolution stopPlayback()
    {
      return PlaybackCursor::CommandResolution{.action = PlaybackCursor::CommandAction::Stop};
    }

    PlaybackCursor::CommandResolution noCommand()
    {
      return PlaybackCursor::CommandResolution{.action = PlaybackCursor::CommandAction::NoOp};
    }
  } // namespace

  PlaybackCursor::PlaybackCursor(PlaybackLaunchSpec launchSpec,
                                 ProjectionAnchor currentAnchor,
                                 RepeatMode const repeatMode,
                                 ShuffleMode const shuffleMode,
                                 PlaybackCursorPolicy& policy)
    : _launchSpec{std::move(launchSpec)}
    , _currentTrackId{currentAnchor.trackId()}
    , _anchor{std::move(currentAnchor)}
    , _repeatMode{repeatMode}
    , _shuffleMode{shuffleMode}
  {
    if (_launchSpec.sourceListId == kInvalidListId)
    {
      throwException<Exception>("Playback cursor requires a valid launch source");
    }

    validateLiveAnchor(_anchor, policy);
    _semanticTuple = computeSemanticTuple(policy);
  }

  PlaybackCursor::Changes PlaybackCursor::applyProjectionBatch(TrackListProjectionDeltaBatch const& batch,
                                                               PlaybackCursorPolicy& policy)
  {
    if (_sourceState == SourceState::Invalidated)
    {
      throwException<Exception>("Invalidated playback cursor cannot consume projection batches");
    }

    auto const previousAnchorIndex = _anchor.anchorIndex();
    auto const projectionSize = policy.projectionSize();
    auto const optCurrentIndex = policy.indexOf(_currentTrackId);
    _anchor.applyBatch(batch, projectionSize, optCurrentIndex);
    return changes(_anchor.anchorIndex() != previousAnchorIndex, policy);
  }

  PlaybackCursor::Changes PlaybackCursor::invalidateSource(PlaybackCursorPolicy& policy)
  {
    if (_sourceState == SourceState::Invalidated)
    {
      return {};
    }

    policy.clearShuffleState();
    _sourceState = SourceState::Invalidated;
    return changes(false, policy);
  }

  PlaybackCursor::Changes PlaybackCursor::setRepeatMode(RepeatMode const mode, PlaybackCursorPolicy& policy)
  {
    if (_repeatMode == mode)
    {
      return {};
    }

    _repeatMode = mode;

    if (_sourceState == SourceState::Live)
    {
      policy.invalidateShuffleForwardCandidate();
    }

    return changes(true, policy);
  }

  PlaybackCursor::Changes PlaybackCursor::setShuffleMode(ShuffleMode const mode, PlaybackCursorPolicy& policy)
  {
    if (_shuffleMode == mode)
    {
      return {};
    }

    _shuffleMode = mode;

    if (_sourceState == SourceState::Live)
    {
      policy.invalidateShuffleForwardCandidate();
    }

    return changes(true, policy);
  }

  PlaybackCursor::Changes PlaybackCursor::setPreviousRestartAvailable(bool const available,
                                                                      PlaybackCursorPolicy& policy)
  {
    if (_previousRestartAvailable == available)
    {
      return {};
    }

    _previousRestartAvailable = available;
    return changes(false, policy);
  }

  PlaybackCursor::Changes PlaybackCursor::adoptLiveCurrent(ProjectionAnchor currentAnchor, PlaybackCursorPolicy& policy)
  {
    if (_sourceState == SourceState::Invalidated)
    {
      throwException<Exception>("Invalidated playback cursor cannot adopt a live anchor");
    }

    validateLiveAnchor(currentAnchor, policy);
    auto const restorableStateChanged =
      currentAnchor.trackId() != _currentTrackId || currentAnchor.anchorIndex() != _anchor.anchorIndex();

    if (currentAnchor.trackId() != _currentTrackId)
    {
      policy.invalidateShuffleForwardCandidate();
    }

    _currentTrackId = currentAnchor.trackId();
    _anchor = std::move(currentAnchor);
    return changes(restorableStateChanged, policy);
  }

  PlaybackCursor::Changes PlaybackCursor::adoptInvalidatedCurrent(TrackId const currentTrackId)
  {
    if (_sourceState != SourceState::Invalidated)
    {
      throwException<Exception>("Live playback cursor requires a projection anchor for a current transition");
    }

    if (currentTrackId == kInvalidTrackId)
    {
      throwException<Exception>("Playback cursor requires a valid current track");
    }

    if (_currentTrackId == currentTrackId)
    {
      return {};
    }

    _currentTrackId = currentTrackId;
    auto const nextTuple = SemanticTuple{
      .sourceState = SourceState::Invalidated,
      .currentTrackId = _currentTrackId,
      .hasNext = false,
      .hasPrevious = _previousRestartAvailable,
      .optResolvedSuccessor = std::nullopt,
    };
    auto const semanticChanged = nextTuple != _semanticTuple;

    if (semanticChanged)
    {
      _semanticTuple = nextTuple;
    }

    return Changes{.semanticChanged = semanticChanged, .restorableStateChanged = true};
  }

  PlaybackCursor::Changes PlaybackCursor::refreshSemanticState(PlaybackCursorPolicy& policy)
  {
    return changes(false, policy);
  }

  PlaybackCursor::CommandResolution PlaybackCursor::resolveNext() const noexcept
  {
    if (_sourceState == SourceState::Invalidated)
    {
      return stopPlayback();
    }

    if (_semanticTuple.optResolvedSuccessor)
    {
      return startTrack(*_semanticTuple.optResolvedSuccessor);
    }

    return stopPlayback();
  }

  PlaybackCursor::CommandResolution PlaybackCursor::resolvePrevious(PlaybackCursorPolicy& policy)
  {
    if (_sourceState == SourceState::Invalidated)
    {
      return _previousRestartAvailable ? restartCurrent(_currentTrackId) : noCommand();
    }

    if (_previousRestartAvailable)
    {
      return restartCurrent(_currentTrackId);
    }

    if (_shuffleMode == ShuffleMode::On)
    {
      if (auto const optPrevious = policy.popShufflePrevious(_currentTrackId); optPrevious)
      {
        if (*optPrevious == kInvalidTrackId)
        {
          throwException<Exception>("Shuffle previous returned an invalid track");
        }

        return startTrack(*optPrevious);
      }

      return noCommand();
    }

    if (auto const optPrevious = resolveSequentialPrevious(policy); optPrevious)
    {
      return startTrack(*optPrevious);
    }

    return noCommand();
  }

  void PlaybackCursor::validateLiveAnchor(ProjectionAnchor const& anchor, PlaybackCursorPolicy& policy) const
  {
    auto const projectionSize = policy.projectionSize();
    auto const optIndex = policy.indexOf(anchor.trackId());

    if (anchor.anchorIndex() > projectionSize)
    {
      throwException<Exception>("Playback cursor anchor exceeds the projection size");
    }

    if (optIndex)
    {
      if (*optIndex >= projectionSize || anchor.state() != ProjectionAnchor::State::Bound ||
          anchor.anchorIndex() != *optIndex)
      {
        throwException<Exception>("Playback cursor Bound anchor does not match projection identity");
      }

      return;
    }

    if (anchor.state() != ProjectionAnchor::State::Gap)
    {
      throwException<Exception>("Playback cursor Gap anchor is required for a missing projected current");
    }
  }

  PlaybackCursor::SemanticTuple PlaybackCursor::computeSemanticTuple(PlaybackCursorPolicy& policy)
  {
    auto tuple = SemanticTuple{.sourceState = _sourceState, .currentTrackId = _currentTrackId};

    if (_sourceState == SourceState::Invalidated)
    {
      tuple.hasPrevious = _previousRestartAvailable;
      return tuple;
    }

    tuple.optResolvedSuccessor = resolveSuccessor(policy);
    tuple.hasNext = tuple.optResolvedSuccessor.has_value();

    if (_previousRestartAvailable)
    {
      tuple.hasPrevious = true;
    }
    else if (_shuffleMode == ShuffleMode::On)
    {
      tuple.hasPrevious = policy.hasShufflePrevious(_currentTrackId);
    }
    else
    {
      tuple.hasPrevious = resolveSequentialPrevious(policy).has_value();
    }

    return tuple;
  }

  std::optional<TrackId> PlaybackCursor::resolveSuccessor(PlaybackCursorPolicy& policy)
  {
    if (_repeatMode == RepeatMode::One)
    {
      return _currentTrackId;
    }

    if (_shuffleMode == ShuffleMode::On)
    {
      auto const optCandidate = policy.shuffleForwardCandidate(
        _currentTrackId, _anchor.state() == ProjectionAnchor::State::Bound, _repeatMode == RepeatMode::All);

      if (optCandidate && *optCandidate == kInvalidTrackId)
      {
        throwException<Exception>("Shuffle forward returned an invalid track");
      }

      return optCandidate;
    }

    return resolveSequentialSuccessor(policy);
  }

  std::optional<TrackId> PlaybackCursor::resolveSequentialSuccessor(PlaybackCursorPolicy& policy) const
  {
    auto const projectionSize = policy.projectionSize();
    auto const candidateIndex =
      _anchor.state() == ProjectionAnchor::State::Bound ? _anchor.anchorIndex() + 1 : _anchor.anchorIndex();

    if (candidateIndex < projectionSize)
    {
      return requireTrackAt(policy, candidateIndex);
    }

    if (_repeatMode == RepeatMode::All && projectionSize > 0)
    {
      return requireTrackAt(policy, 0);
    }

    return std::nullopt;
  }

  std::optional<TrackId> PlaybackCursor::resolveSequentialPrevious(PlaybackCursorPolicy& policy) const
  {
    auto const projectionSize = policy.projectionSize();

    if (_anchor.anchorIndex() > 0)
    {
      return requireTrackAt(policy, _anchor.anchorIndex() - 1);
    }

    if (_repeatMode == RepeatMode::All && projectionSize > 0)
    {
      return requireTrackAt(policy, projectionSize - 1);
    }

    return std::nullopt;
  }

  TrackId PlaybackCursor::requireTrackAt(PlaybackCursorPolicy& policy, std::size_t const index) const
  {
    auto const trackId = policy.trackIdAt(index);

    if (trackId == kInvalidTrackId)
    {
      throwException<Exception>("Playback cursor resolved an invalid projected track");
    }

    return trackId;
  }

  bool PlaybackCursor::updateSemanticTuple(PlaybackCursorPolicy& policy)
  {
    auto const nextTuple = computeSemanticTuple(policy);

    if (nextTuple == _semanticTuple)
    {
      return false;
    }

    _semanticTuple = nextTuple;
    return true;
  }

  PlaybackCursor::Changes PlaybackCursor::changes(bool const restorableStateChanged, PlaybackCursorPolicy& policy)
  {
    return Changes{
      .semanticChanged = updateSemanticTuple(policy),
      .restorableStateChanged = restorableStateChanged,
    };
  }
} // namespace ao::rt
