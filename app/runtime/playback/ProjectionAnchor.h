// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>

#include <cstddef>
#include <cstdint>
#include <optional>

namespace ao::rt
{
  struct TrackListProjectionDeltaBatch;

  class ProjectionAnchor final
  {
  public:
    enum class State : std::uint8_t
    {
      Bound,
      Gap,
    };

    static ProjectionAnchor bound(TrackId trackId, std::size_t anchorIndex, std::size_t projectionSize);
    static ProjectionAnchor gap(TrackId trackId, std::size_t anchorIndex, std::size_t projectionSize);

    TrackId trackId() const noexcept { return _trackId; }
    State state() const noexcept { return _state; }
    std::size_t anchorIndex() const noexcept { return _anchorIndex; }

    /**
     * Applies a complete sequential delta batch, then reconciles track identity
     * against the final projection snapshot.
     *
     * optTrackIndex is indexOf(trackId()) in the post-batch projection. It must
     * be absent when the current track is outside that projection.
     */
    void applyBatch(TrackListProjectionDeltaBatch const& batch,
                    std::size_t projectionSize,
                    std::optional<std::size_t> optTrackIndex);

    bool operator==(ProjectionAnchor const&) const = default;

  private:
    ProjectionAnchor(TrackId trackId, State state, std::size_t anchorIndex);

    void applyInsert(std::size_t start, std::size_t count);
    void applyRemove(std::size_t start, std::size_t count);
    void reconcile(std::size_t projectionSize, std::optional<std::size_t> optTrackIndex);

    TrackId _trackId = kInvalidTrackId;
    State _state = State::Gap;
    std::size_t _anchorIndex = 0;
  };
} // namespace ao::rt
