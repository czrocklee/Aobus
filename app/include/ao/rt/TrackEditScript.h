// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/Error.h>

#include <cstddef>
#include <span>
#include <variant>
#include <vector>

namespace ao::rt::delta
{
  struct InsertRange final
  {
    std::size_t start = 0;
    std::vector<TrackId> trackIds{};

    bool operator==(InsertRange const&) const = default;
  };

  struct RemoveRange final
  {
    std::size_t start = 0;
    std::vector<TrackId> trackIds{};

    bool operator==(RemoveRange const&) const = default;
  };

  struct UpdateRange final
  {
    std::size_t start = 0;
    std::vector<TrackId> trackIds{};

    bool operator==(UpdateRange const&) const = default;
  };

  using RegularTrackEdit = std::variant<InsertRange, RemoveRange, UpdateRange>;

  struct RegularTrackEditScript final
  {
    std::vector<RegularTrackEdit> edits{};

    bool operator==(RegularTrackEditScript const&) const = default;
  };

  /**
   * Canonical scripts contain descending removals, then ascending insertions,
   * then ascending updates. Removal coordinates address the initial sequence;
   * insertion coordinates address the sequential post-removal result; update
   * coordinates address the final sequence.
   */
  bool validate(RegularTrackEditScript const& script, std::size_t initialSize) noexcept;

  /** Applies a canonical script with identity checks in O(n + k). */
  Result<std::vector<TrackId>> apply(std::vector<TrackId> initial, RegularTrackEditScript const& script);

  /**
   * Diffs unique-id sequences. A longest common subsequence is retained;
   * common members outside it are represented as remove+insert moves.
   */
  RegularTrackEditScript diff(std::span<TrackId const> from,
                              std::span<TrackId const> to,
                              std::span<TrackId const> updatedIds = {},
                              std::span<TrackId const> preferredMovedIds = {});

  class Coalescer final
  {
  public:
    void appendRemove(std::size_t start, std::span<TrackId const> trackIds);
    void appendInsert(std::size_t start, std::span<TrackId const> trackIds);
    void appendUpdate(std::size_t start, std::span<TrackId const> trackIds);

    RegularTrackEditScript take();

  private:
    RegularTrackEditScript _script;
  };
} // namespace ao::rt::delta
