// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include "runtime/playback/ProjectionAnchor.h"
#include <ao/CoreIds.h>
#include <ao/rt/PlaybackLaunchSpec.h>
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

    struct Changes final
    {
      bool semanticChanged = false;
      bool restorableStateChanged = false;

      bool operator==(Changes const&) const = default;
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

    PlaybackCursor(PlaybackLaunchSpec launchSpec,
                   ProjectionAnchor currentAnchor,
                   RepeatMode repeatMode,
                   ShuffleMode shuffleMode,
                   PlaybackCursorPolicy& policy);

    PlaybackLaunchSpec const& launchSpec() const noexcept { return _launchSpec; }
    SourceState sourceState() const noexcept { return _sourceState; }
    TrackId currentTrackId() const noexcept { return _currentTrackId; }
    ProjectionAnchor const& anchor() const noexcept { return _anchor; }
    RepeatMode repeatMode() const noexcept { return _repeatMode; }
    ShuffleMode shuffleMode() const noexcept { return _shuffleMode; }
    bool previousRestartAvailable() const noexcept { return _previousRestartAvailable; }
    SemanticTuple const& semanticTuple() const noexcept { return _semanticTuple; }

    /** Applies one complete regular projection batch and reconciles once. */
    Changes applyProjectionBatch(TrackListProjectionDeltaBatch const& batch, PlaybackCursorPolicy& policy);

    /** Freezes sequence authority without changing restorable cursor state. */
    Changes invalidateSource(PlaybackCursorPolicy& policy);

    Changes setRepeatMode(RepeatMode mode, PlaybackCursorPolicy& policy);
    Changes setShuffleMode(ShuffleMode mode, PlaybackCursorPolicy& policy);

    /**
     * Updates the elapsed-time restart policy. Elapsed progress is not part of
     * restorable cursor state.
     */
    Changes setPreviousRestartAvailable(bool available, PlaybackCursorPolicy& policy);

    /** Adopts an already-reconciled current anchor while the source is Live. */
    Changes adoptLiveCurrent(ProjectionAnchor currentAnchor, PlaybackCursorPolicy& policy);

    /**
     * Updates only the present transport subject after source invalidation;
     * the non-authoritative anchor remains frozen and no policy is queried.
     */
    Changes adoptInvalidatedCurrent(TrackId currentTrackId);

    /** Re-evaluates after transient policy state changes such as a failed shuffle candidate. */
    Changes refreshSemanticState(PlaybackCursorPolicy& policy);

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
    Changes changes(bool restorableStateChanged, PlaybackCursorPolicy& policy);

    PlaybackLaunchSpec const _launchSpec;
    SourceState _sourceState = SourceState::Live;
    TrackId _currentTrackId = kInvalidTrackId;
    ProjectionAnchor _anchor;
    RepeatMode _repeatMode = RepeatMode::Off;
    ShuffleMode _shuffleMode = ShuffleMode::Off;
    bool _previousRestartAvailable = false;
    SemanticTuple _semanticTuple{};
  };
} // namespace ao::rt
