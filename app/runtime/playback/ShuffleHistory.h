// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

namespace ao::rt
{
  class ShuffleHistory final
  {
  public:
    static constexpr std::size_t kHistoryCapacity = 64;

    using CandidateChooser = std::move_only_function<TrackId(std::span<TrackId const>)>;

    enum class TransitionOrigin : std::uint8_t
    {
      Forward,
      SequentialPrevious,
      HistoryPrevious,
      Restart,
    };

    explicit ShuffleHistory(CandidateChooser candidateChooser);

    /**
     * Returns one sticky shuffle successor from the current projection.
     *
     * A Bound current is excluded. A Gap current remains eligible like every
     * other projected member. repeatAll permits replaying a sole Bound current
     * when no alternative exists. Repeat-one is resolved before this model.
     */
    std::optional<TrackId> forwardCandidate(std::span<TrackId const> projection,
                                            TrackId currentTrackId,
                                            bool currentIsBound,
                                            bool repeatAll,
                                            std::span<TrackId const> excludedTrackIds = {});

    /**
     * Relevant repeat/shuffle mode changes invalidate the sticky candidate.
     * History remains intact because it records the actual navigation path.
     */
    void invalidateForwardCandidate() noexcept;

    /**
     * Forgets an exact candidate after preparation or playback failure. A
     * stale failure for a different candidate leaves the current one intact.
     */
    bool discardForwardCandidate(TrackId candidate) noexcept;

    /**
     * Records a successful current-track transition. Forward and sequential
     * previous navigation push the track being left. History-backed previous,
     * restart, and same-track replay do not push.
     */
    void recordTransition(TrackId leavingTrackId, TrackId arrivingTrackId, TransitionOrigin origin);

    /** Scans for a valid history candidate without changing the stack. */
    bool hasPrevious(std::span<TrackId const> projection, TrackId currentTrackId) const noexcept;

    /**
     * Pops until a projected candidate different from the current track is
     * found. The returned entry is already removed, so a failed start is
     * discarded and the caller may continue the ordinary bounded failure walk.
     */
    std::optional<TrackId> popPrevious(std::span<TrackId const> projection, TrackId currentTrackId);

    /** Clears transient candidate and history state on session invalidation. */
    void clear() noexcept;

    std::optional<TrackId> pendingForwardCandidate() const noexcept { return _optForwardCandidate; }
    std::size_t historySize() const noexcept { return _history.size(); }

  private:
    CandidateChooser _candidateChooser;
    std::optional<TrackId> _optForwardCandidate;
    std::vector<TrackId> _history;
  };
} // namespace ao::rt
