// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include "runtime/playback/PlaybackCursor.h"
#include "runtime/playback/PreparedNextRegistry.h"
#include "runtime/playback/ProjectionAnchor.h"
#include "runtime/playback/ShuffleHistory.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/rt/PlaybackLaunchContext.h>
#include <ao/rt/PlaybackMode.h>
#include <ao/rt/PreparedPlayback.h>
#include <ao/rt/Subscription.h>
#include <ao/rt/source/TrackSourceLease.h>

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace ao::library
{
  class MusicLibrary;
}

namespace ao::rt
{
  class LiveTrackListProjection;
  class SmartListSource;
  class TrackSourceCache;
  struct TrackListProjectionDeltaBatch;

  /** Owns one bounded live-list playback source/projection conversation. */
  class [[nodiscard]] PlaybackCursorSession final : private PlaybackCursorPolicy
  {
  public:
    using ProjectionBatchHandler =
      std::move_only_function<void(PlaybackCursor::MutationEffect effect, bool sourceInvalidated)>;

    static Result<std::unique_ptr<PlaybackCursorSession>> create(PlaybackLaunchContext launchContext,
                                                                 TrackId startTrackId,
                                                                 TrackSourceCache& sources,
                                                                 library::MusicLibrary& library,
                                                                 RepeatMode repeatMode,
                                                                 ShuffleMode shuffleMode,
                                                                 ShuffleHistory::CandidateChooser candidateChooser);

    /** Builds a non-playing restore candidate, allowing the saved current track to be a projection gap. */
    static Result<std::unique_ptr<PlaybackCursorSession>> createForRestore(
      PlaybackLaunchContext launchContext,
      TrackId currentTrackId,
      std::size_t anchorIndex,
      TrackSourceCache& sources,
      library::MusicLibrary& library,
      RepeatMode repeatMode,
      ShuffleMode shuffleMode,
      ShuffleHistory::CandidateChooser candidateChooser);

    ~PlaybackCursorSession() override;

    PlaybackCursorSession(PlaybackCursorSession const&) = delete;
    PlaybackCursorSession& operator=(PlaybackCursorSession const&) = delete;
    PlaybackCursorSession(PlaybackCursorSession&&) = delete;
    PlaybackCursorSession& operator=(PlaybackCursorSession&&) = delete;

    /** Starts observation only after the service owns this complete session. */
    void startObserving(ProjectionBatchHandler handler);

    PlaybackCursor& cursor() noexcept { return _cursor; }
    PlaybackCursor const& cursor() const noexcept { return _cursor; }
    PreparedNextRegistry& preparedNextRegistry() noexcept { return _preparedNextRegistry; }
    PreparedNextRegistry const& preparedNextRegistry() const noexcept { return _preparedNextRegistry; }
    ShuffleHistory& shuffleHistory() noexcept { return _shuffleHistory; }
    std::optional<TrackId> rerollShuffleForward(std::span<TrackId const> excludedTrackIds);

    // Public snapshot access is also the private cursor-policy implementation.
    // NOLINTNEXTLINE(misc-override-with-different-visibility)
    std::size_t projectionSize() const override;
    // NOLINTNEXTLINE(misc-override-with-different-visibility)
    TrackId trackIdAt(std::size_t index) const override;
    // NOLINTNEXTLINE(misc-override-with-different-visibility)
    std::optional<std::size_t> indexOf(TrackId trackId) const override;

    ProjectionAnchor anchorFor(TrackId trackId, std::size_t fallbackGap) const;
    PlaybackCursor::MutationEffect refreshSemanticState();
    PlaybackCursor::MutationEffect setPreviousRestartAvailable(bool available);
    PlaybackCursor::MutationEffect setShuffleMode(ShuffleMode mode);
    PlaybackCursor::MutationEffect setRepeatMode(RepeatMode mode);
    PlaybackCursor::CommandResolution resolvePrevious();
    Result<PlaybackCursor::MutationEffect> adoptCurrent(TrackId trackId,
                                                        std::optional<PreparedNextToken> optPreparedNextToken,
                                                        ShuffleHistory::TransitionOrigin origin);

    void invalidatePreparedNext(std::optional<PreparedNextToken> optDisarmedToken);
    void clearPreparedCoveredBy(PreparedCancellationBarrier barrier) noexcept;

    std::size_t consecutiveFailureCount() const noexcept { return _consecutiveFailureCount; }
    std::size_t incrementConsecutiveFailureCount() noexcept { return ++_consecutiveFailureCount; }
    void resetConsecutiveFailureCount() noexcept { _consecutiveFailureCount = 0; }

    // Construction is exposed only in this runtime-internal header so shared launch/restore factories can build it.
    PlaybackCursorSession(PlaybackLaunchContext launchContext,
                          TrackSourceLease baseSourceLease,
                          std::shared_ptr<SmartListSource> quickFilterSourcePtr,
                          std::unique_ptr<LiveTrackListProjection> projectionPtr,
                          ProjectionAnchor currentAnchor,
                          RepeatMode repeatMode,
                          ShuffleMode shuffleMode,
                          ShuffleHistory::CandidateChooser candidateChooser);

  private:
    void handleProjectionBatch(TrackListProjectionDeltaBatch const& batch);
    std::vector<TrackId> projectionSnapshot() const;

    std::optional<TrackId> shuffleForwardCandidate(TrackId currentTrackId,
                                                   bool currentIsBound,
                                                   bool repeatAll) override;
    bool hasShufflePrevious(TrackId currentTrackId) const override;
    std::optional<TrackId> popShufflePrevious(TrackId currentTrackId) override;
    void invalidateShuffleForwardCandidate() noexcept override;
    void clearShuffleState() noexcept override;

    TrackSourceLease _baseSourceLease;
    std::shared_ptr<SmartListSource> _quickFilterSourcePtr;
    std::unique_ptr<LiveTrackListProjection> _projectionPtr;
    ShuffleHistory _shuffleHistory;
    PlaybackCursor _cursor;
    PreparedNextRegistry _preparedNextRegistry;
    ProjectionBatchHandler _projectionBatchHandler;
    Subscription _projectionSubscription;
    std::size_t _consecutiveFailureCount = 0;
  };
} // namespace ao::rt
