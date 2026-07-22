// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include "runtime/playback/ProjectionAnchor.h"
#include <ao/CoreIds.h>
#include <ao/rt/PreparedPlayback.h>

#include <cstddef>
#include <functional>
#include <optional>
#include <vector>

namespace ao::rt
{
  struct TrackListProjectionDeltaBatch;

  class PreparedNextRegistry final
  {
  public:
    using ProjectionIndexResolver = std::function<std::optional<std::size_t>(TrackId)>;

    void activate(PreparedNextToken token, ProjectionAnchor anchor);
    bool acknowledgeDisarm(PreparedNextToken token);

    /**
     * Retires an unacknowledged active token while forgetting an exact token
     * that the engine proved was disarmed.
     */
    void invalidate(std::optional<PreparedNextToken> optDisarmedToken = std::nullopt);

    void applyBatch(TrackListProjectionDeltaBatch const& batch,
                    std::size_t projectionSize,
                    ProjectionIndexResolver const& resolveIndex);

    /**
     * Resolves a prepared transition and closes its token window. A known
     * winner returns its independently maintained anchor and discards every
     * losing active or retired commitment. An unknown token changes nothing.
     */
    std::optional<ProjectionAnchor> resolveWinner(PreparedNextToken token);

    /** Clears only commitments proven unreachable by an audio cancellation barrier. */
    void clearCoveredByBarrier(PreparedCancellationBarrier barrier) noexcept;

    /** Clears a completed token window after a known winner is resolved. */
    void clear() noexcept;

    std::optional<PreparedNextToken> activeToken() const noexcept;
    std::optional<ProjectionAnchor> anchorFor(PreparedNextToken token) const;
    bool contains(PreparedNextToken token) const noexcept;
    bool isRetired(PreparedNextToken token) const noexcept;
    std::size_t size() const noexcept;
    std::size_t retiredCount() const noexcept { return _retired.size(); }

  private:
    struct Commitment final
    {
      PreparedNextToken token{};
      ProjectionAnchor anchor;
    };

    Commitment const* find(PreparedNextToken token) const noexcept;
    void retireActive();

    std::optional<Commitment> _optActive;
    std::vector<Commitment> _retired;
  };
} // namespace ao::rt
