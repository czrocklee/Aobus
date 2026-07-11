// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include "runtime/playback/ProjectionAnchor.h"
#include <ao/CoreIds.h>
#include <ao/rt/PlaybackLaunchContext.h>
#include <ao/rt/PlaybackMode.h>

#include <cstddef>
#include <cstdint>
#include <optional>

namespace ao::rt
{
  struct TrackListProjectionDeltaBatch;

  /**
   * Narrow bridge from the pure cursor model to a session-owned projection
   * and its transient shuffle state.
   */
  class PlaybackCursorPolicy
  {
  public:
    virtual ~PlaybackCursorPolicy() = default;

    PlaybackCursorPolicy(PlaybackCursorPolicy const&) = delete;
    PlaybackCursorPolicy& operator=(PlaybackCursorPolicy const&) = delete;
    PlaybackCursorPolicy(PlaybackCursorPolicy&&) = delete;
    PlaybackCursorPolicy& operator=(PlaybackCursorPolicy&&) = delete;

    virtual std::size_t projectionSize() const = 0;
    virtual TrackId trackIdAt(std::size_t index) const = 0;
    virtual std::optional<std::size_t> indexOf(TrackId trackId) const = 0;

    virtual std::optional<TrackId> shuffleForwardCandidate(TrackId currentTrackId,
                                                           bool currentIsBound,
                                                           bool repeatAll) = 0;
    virtual bool hasShufflePrevious(TrackId currentTrackId) const = 0;
    virtual std::optional<TrackId> popShufflePrevious(TrackId currentTrackId) = 0;

    virtual void invalidateShuffleForwardCandidate() noexcept = 0;
    virtual void clearShuffleState() noexcept = 0;

  protected:
    PlaybackCursorPolicy() = default;
  };

  class PlaybackCursor final
  {
  public:
    enum class SourceState : std::uint8_t
    {
      Live,
      Invalidated,
    };

    struct SemanticTuple final
    {
      SourceState sourceState = SourceState::Live;
      TrackId currentTrackId = kInvalidTrackId;
      bool hasNext = false;
      bool hasPrevious = false;
      std::optional<TrackId> optResolvedSuccessor{};

      bool operator==(SemanticTuple const&) const = default;
    };

    struct MutationEffect final
    {
      bool semanticChanged = false;
      bool persistenceIntentChanged = false;

      bool operator==(MutationEffect const&) const = default;
    };

    enum class CommandAction : std::uint8_t
    {
      StartTrack,
      RestartCurrent,
      Stop,
      NoOp,
    };

    struct CommandResolution final
    {
      CommandAction action = CommandAction::NoOp;
      TrackId trackId = kInvalidTrackId;

      bool operator==(CommandResolution const&) const = default;
    };

    PlaybackCursor(PlaybackLaunchContext launchContext,
                   ProjectionAnchor currentAnchor,
                   RepeatMode repeatMode,
                   ShuffleMode shuffleMode,
                   PlaybackCursorPolicy& policy);

    PlaybackLaunchContext const& launchContext() const noexcept { return _launchContext; }
    SourceState sourceState() const noexcept { return _sourceState; }
    TrackId currentTrackId() const noexcept { return _currentTrackId; }
    ProjectionAnchor const& anchor() const noexcept { return _anchor; }
    RepeatMode repeatMode() const noexcept { return _repeatMode; }
    ShuffleMode shuffleMode() const noexcept { return _shuffleMode; }
    bool previousRestartAvailable() const noexcept { return _previousRestartAvailable; }
    SemanticTuple const& semanticTuple() const noexcept { return _semanticTuple; }
    std::uint64_t semanticRevision() const noexcept { return _semanticRevision; }

    /** Applies one complete regular projection batch and reconciles once. */
    MutationEffect applyProjectionBatch(TrackListProjectionDeltaBatch const& batch, PlaybackCursorPolicy& policy);

    /** Freezes sequence authority without changing serialized cursor intent. */
    MutationEffect invalidateSource(PlaybackCursorPolicy& policy);

    MutationEffect setRepeatMode(RepeatMode mode, PlaybackCursorPolicy& policy);
    MutationEffect setShuffleMode(ShuffleMode mode, PlaybackCursorPolicy& policy);

    /**
     * Updates the elapsed-time restart policy. Elapsed progress is not
     * serialized cursor intent.
     */
    MutationEffect setPreviousRestartAvailable(bool available, PlaybackCursorPolicy& policy);

    /** Adopts an already-reconciled current anchor while the source is Live. */
    MutationEffect adoptLiveCurrent(ProjectionAnchor currentAnchor, PlaybackCursorPolicy& policy);

    /**
     * Updates only the present transport subject after source invalidation;
     * the non-authoritative anchor remains frozen and no policy is queried.
     */
    MutationEffect adoptInvalidatedCurrent(TrackId currentTrackId);

    /** Re-evaluates after transient policy state changes such as a failed shuffle candidate. */
    MutationEffect refreshSemanticState(PlaybackCursorPolicy& policy);

    /** Resolves both manual next and natural advance. */
    CommandResolution resolveNext() const noexcept;
    CommandResolution resolveNaturalAdvance() const noexcept { return resolveNext(); }
    CommandResolution resolvePrevious(PlaybackCursorPolicy& policy);

  private:
    void validateLiveAnchor(ProjectionAnchor const& anchor, PlaybackCursorPolicy& policy) const;
    SemanticTuple computeSemanticTuple(PlaybackCursorPolicy& policy);
    std::optional<TrackId> resolveSuccessor(PlaybackCursorPolicy& policy);
    std::optional<TrackId> resolveSequentialSuccessor(PlaybackCursorPolicy& policy) const;
    std::optional<TrackId> resolveSequentialPrevious(PlaybackCursorPolicy& policy) const;
    TrackId requireTrackAt(PlaybackCursorPolicy& policy, std::size_t index) const;
    bool updateSemanticTuple(PlaybackCursorPolicy& policy);
    MutationEffect effect(bool persistenceIntentChanged, PlaybackCursorPolicy& policy);

    PlaybackLaunchContext const _launchContext;
    SourceState _sourceState = SourceState::Live;
    TrackId _currentTrackId = kInvalidTrackId;
    ProjectionAnchor _anchor;
    RepeatMode _repeatMode = RepeatMode::Off;
    ShuffleMode _shuffleMode = ShuffleMode::Off;
    bool _previousRestartAvailable = false;
    SemanticTuple _semanticTuple{};
    std::uint64_t _semanticRevision = 0;
  };
} // namespace ao::rt
