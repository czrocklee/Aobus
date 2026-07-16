// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "runtime/playback/PlaybackCursorSession.h"

#include "runtime/playback/ProjectionAnchor.h"
#include "runtime/playback/ShuffleHistory.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/Exception.h>
#include <ao/library/MusicLibrary.h>
#include <ao/rt/PlaybackLaunchSpec.h>
#include <ao/rt/PlaybackMode.h>
#include <ao/rt/PreparedPlayback.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/projection/LiveTrackListProjection.h>
#include <ao/rt/projection/TrackListProjection.h>
#include <ao/rt/source/TrackSource.h>
#include <ao/rt/source/TrackSourceCache.h>

#include <algorithm>
#include <cstddef>
#include <exception>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <variant>
#include <vector>

namespace ao::rt
{
  namespace
  {
    Result<std::unique_ptr<PlaybackCursorSession>> buildSession(
      PlaybackLaunchSpec launchSpec,
      TrackId const currentTrackId,
      std::optional<std::size_t> const optRequiredCurrentIndex,
      std::size_t const fallbackAnchorIndex,
      TrackSourceCache& sources,
      library::MusicLibrary const& library,
      RepeatMode const repeatMode,
      ShuffleMode const shuffleMode,
      ShuffleHistory::CandidateChooser candidateChooser)
    {
      if (launchSpec.sourceListId == kInvalidListId || currentTrackId == kInvalidTrackId)
      {
        return makeError(Error::Code::InvalidInput, "Playback launch requires valid source and track identities");
      }

      auto baseSourceResult = sources.acquire(launchSpec.sourceListId);

      if (!baseSourceResult)
      {
        return std::unexpected{baseSourceResult.error()};
      }

      try
      {
        auto baseSourceLease = std::move(*baseSourceResult);
        auto projectionSourceLease = baseSourceLease;

        if (!launchSpec.quickFilterExpression.empty())
        {
          auto filteredResult = sources.acquire(
            SourceSpec{.baseListId = launchSpec.sourceListId, .filterExpression = launchSpec.quickFilterExpression});

          if (!filteredResult)
          {
            return std::unexpected{filteredResult.error()};
          }

          projectionSourceLease = std::move(*filteredResult);

          if (auto const optError = sources.sourceError(projectionSourceLease); optError)
          {
            return std::unexpected{*optError};
          }
        }

        if (projectionSourceLease->state() == TrackSourceState::Invalidated)
        {
          return makeError(Error::Code::InvalidState, "Playback source was invalidated during launch");
        }

        auto projectionPtr =
          std::make_unique<LiveTrackListProjection>(kInvalidViewId, projectionSourceLease, library, launchSpec.order);
        auto const optCurrentIndex = projectionPtr->indexOf(currentTrackId);

        if (optRequiredCurrentIndex && !optCurrentIndex)
        {
          return makeError(Error::Code::NotFound, "Start track is not present in the captured playback projection");
        }

        auto currentAnchor =
          optCurrentIndex
            ? ProjectionAnchor::bound(currentTrackId, *optCurrentIndex, projectionPtr->size())
            : ProjectionAnchor::gap(
                currentTrackId, std::min(fallbackAnchorIndex, projectionPtr->size()), projectionPtr->size());
        return std::make_unique<PlaybackCursorSession>(std::move(launchSpec),
                                                       std::move(baseSourceLease),
                                                       std::move(projectionPtr),
                                                       std::move(currentAnchor),
                                                       repeatMode,
                                                       shuffleMode,
                                                       std::move(candidateChooser));
      }
      catch (std::exception const& error)
      {
        return makeError(Error::Code::Generic, error.what());
      }
    }
  } // namespace

  Result<std::unique_ptr<PlaybackCursorSession>> PlaybackCursorSession::create(
    PlaybackLaunchSpec launchSpec,
    TrackId const startTrackId,
    TrackSourceCache& sources,
    library::MusicLibrary const& library,
    RepeatMode const repeatMode,
    ShuffleMode const shuffleMode,
    ShuffleHistory::CandidateChooser candidateChooser)
  {
    return buildSession(std::move(launchSpec),
                        startTrackId,
                        std::size_t{0},
                        0,
                        sources,
                        library,
                        repeatMode,
                        shuffleMode,
                        std::move(candidateChooser));
  }

  Result<std::unique_ptr<PlaybackCursorSession>> PlaybackCursorSession::createForRestore(
    PlaybackLaunchSpec launchSpec,
    TrackId const currentTrackId,
    std::size_t const anchorIndex,
    TrackSourceCache& sources,
    library::MusicLibrary const& library,
    RepeatMode const repeatMode,
    ShuffleMode const shuffleMode,
    ShuffleHistory::CandidateChooser candidateChooser)
  {
    return buildSession(std::move(launchSpec),
                        currentTrackId,
                        std::nullopt,
                        anchorIndex,
                        sources,
                        library,
                        repeatMode,
                        shuffleMode,
                        std::move(candidateChooser));
  }

  PlaybackCursorSession::PlaybackCursorSession(PlaybackLaunchSpec launchSpec,
                                               TrackSourceLease baseSourceLease,
                                               std::unique_ptr<LiveTrackListProjection> projectionPtr,
                                               ProjectionAnchor currentAnchor,
                                               RepeatMode const repeatMode,
                                               ShuffleMode const shuffleMode,
                                               ShuffleHistory::CandidateChooser candidateChooser)
    : _baseSourceLease{std::move(baseSourceLease)}
    , _projectionPtr{std::move(projectionPtr)}
    , _shuffleHistory{std::move(candidateChooser)}
    , _cursor{std::move(launchSpec), std::move(currentAnchor), repeatMode, shuffleMode, *this}
  {
  }

  PlaybackCursorSession::~PlaybackCursorSession() = default;

  void PlaybackCursorSession::startObserving(ProjectionBatchHandler handler)
  {
    if (_projectionSubscription)
    {
      throwException<Exception>("Playback cursor session is already observing its projection");
    }

    if (!handler)
    {
      throwException<Exception>("Playback cursor session requires a projection batch handler");
    }

    _projectionBatchHandler = std::move(handler);
    _projectionSubscription =
      _projectionPtr->subscribe([this](TrackListProjectionDeltaBatch const& batch) { handleProjectionBatch(batch); });
  }

  std::size_t PlaybackCursorSession::projectionSize() const
  {
    return _projectionPtr->size();
  }

  TrackId PlaybackCursorSession::trackIdAt(std::size_t const index) const
  {
    return _projectionPtr->trackIdAt(index);
  }

  std::optional<std::size_t> PlaybackCursorSession::indexOf(TrackId const trackId) const
  {
    return _projectionPtr->indexOf(trackId);
  }

  ProjectionAnchor PlaybackCursorSession::anchorFor(TrackId const trackId, std::size_t const fallbackGap) const
  {
    if (auto const optIndex = indexOf(trackId); optIndex)
    {
      return ProjectionAnchor::bound(trackId, *optIndex, projectionSize());
    }

    return ProjectionAnchor::gap(trackId, std::min(fallbackGap, projectionSize()), projectionSize());
  }

  PlaybackCursor::MutationEffect PlaybackCursorSession::refreshSemanticState()
  {
    return _cursor.refreshSemanticState(*this);
  }

  PlaybackCursor::MutationEffect PlaybackCursorSession::setPreviousRestartAvailable(bool const available)
  {
    return _cursor.setPreviousRestartAvailable(available, *this);
  }

  PlaybackCursor::MutationEffect PlaybackCursorSession::setShuffleMode(ShuffleMode const mode)
  {
    return _cursor.setShuffleMode(mode, *this);
  }

  PlaybackCursor::MutationEffect PlaybackCursorSession::setRepeatMode(RepeatMode const mode)
  {
    return _cursor.setRepeatMode(mode, *this);
  }

  PlaybackCursor::CommandResolution PlaybackCursorSession::resolvePrevious()
  {
    return _cursor.resolvePrevious(*this);
  }

  std::optional<TrackId> PlaybackCursorSession::rerollShuffleForward(std::span<TrackId const> const excludedTrackIds)
  {
    _shuffleHistory.invalidateForwardCandidate();
    auto const projection = projectionSnapshot();
    return _shuffleHistory.forwardCandidate(projection,
                                            _cursor.currentTrackId(),
                                            _cursor.anchor().state() == ProjectionAnchor::State::Bound,
                                            _cursor.repeatMode() == RepeatMode::All,
                                            excludedTrackIds);
  }

  Result<PlaybackCursor::MutationEffect> PlaybackCursorSession::adoptCurrent(
    TrackId const trackId,
    std::optional<PreparedNextToken> const optPreparedNextToken,
    ShuffleHistory::TransitionOrigin const origin)
  {
    if (trackId == kInvalidTrackId)
    {
      return makeError(Error::Code::InvalidInput, "Playback transition requires a valid track identity");
    }

    auto const leavingTrackId = _cursor.currentTrackId();

    if (_cursor.sourceState() == PlaybackCursor::SourceState::Invalidated)
    {
      if (optPreparedNextToken && !_preparedNextRegistry.resolveWinner(*optPreparedNextToken))
      {
        return makeError(Error::Code::InvalidState, "Prepared transition token is unknown to this playback session");
      }

      if (!optPreparedNextToken)
      {
        _preparedNextRegistry.clear();
      }

      return _cursor.adoptInvalidatedCurrent(trackId);
    }

    auto optAnchor = std::optional<ProjectionAnchor>{};

    if (optPreparedNextToken)
    {
      optAnchor = _preparedNextRegistry.resolveWinner(*optPreparedNextToken);

      if (!optAnchor)
      {
        return makeError(Error::Code::InvalidState, "Prepared transition token is unknown to this playback session");
      }
    }
    else
    {
      _preparedNextRegistry.clear();
      optAnchor = anchorFor(trackId, _cursor.anchor().anchorIndex());
    }

    _shuffleHistory.recordTransition(leavingTrackId, trackId, origin);
    return _cursor.adoptLiveCurrent(std::move(*optAnchor), *this);
  }

  void PlaybackCursorSession::invalidatePreparedNext(std::optional<PreparedNextToken> const optDisarmedToken)
  {
    _preparedNextRegistry.invalidate(optDisarmedToken);
  }

  void PlaybackCursorSession::clearPreparedCoveredBy(PreparedCancellationBarrier const barrier) noexcept
  {
    _preparedNextRegistry.clearCoveredByBarrier(barrier);
  }

  void PlaybackCursorSession::handleProjectionBatch(TrackListProjectionDeltaBatch const& batch)
  {
    auto const sourceInvalidated =
      batch.deltas.size() == 1 && std::holds_alternative<ProjectionSourceInvalidated>(batch.deltas.front());
    auto effect = PlaybackCursor::MutationEffect{};

    if (sourceInvalidated)
    {
      effect = _cursor.invalidateSource(*this);
    }
    else
    {
      _preparedNextRegistry.applyBatch(
        batch, projectionSize(), [this](TrackId const trackId) { return indexOf(trackId); });
      effect = _cursor.applyProjectionBatch(batch, *this);
    }

    _projectionBatchHandler(effect, sourceInvalidated);
  }

  std::vector<TrackId> PlaybackCursorSession::projectionSnapshot() const
  {
    auto trackIds = std::vector<TrackId>{};
    trackIds.reserve(projectionSize());

    for (std::size_t index = 0; index < projectionSize(); ++index)
    {
      trackIds.push_back(trackIdAt(index));
    }

    return trackIds;
  }

  std::optional<TrackId> PlaybackCursorSession::shuffleForwardCandidate(TrackId const currentTrackId,
                                                                        bool const currentIsBound,
                                                                        bool const repeatAll)
  {
    auto const projection = projectionSnapshot();
    return _shuffleHistory.forwardCandidate(projection, currentTrackId, currentIsBound, repeatAll);
  }

  bool PlaybackCursorSession::hasShufflePrevious(TrackId const currentTrackId) const
  {
    auto const projection = projectionSnapshot();
    return _shuffleHistory.hasPrevious(projection, currentTrackId);
  }

  std::optional<TrackId> PlaybackCursorSession::popShufflePrevious(TrackId const currentTrackId)
  {
    auto const projection = projectionSnapshot();
    return _shuffleHistory.popPrevious(projection, currentTrackId);
  }

  void PlaybackCursorSession::invalidateShuffleForwardCandidate() noexcept
  {
    _shuffleHistory.invalidateForwardCandidate();
  }

  void PlaybackCursorSession::clearShuffleState() noexcept
  {
    _shuffleHistory.clear();
  }
} // namespace ao::rt
