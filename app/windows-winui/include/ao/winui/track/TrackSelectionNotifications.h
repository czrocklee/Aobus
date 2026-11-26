// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace ao::winui
{
  struct TrackSelectionSpan
  {
    std::uint32_t first;
    std::uint32_t length;
  };

  enum class TrackSelectionEventKind : std::uint8_t
  {
    Selected,
    Added,
    Removed,
    Invalidated
  };

  struct TrackSelectionEvent
  {
    TrackSelectionEventKind kind;
    std::uint32_t index;

    bool operator==(TrackSelectionEvent const&) const = default;
  };

  // Both selections must use the same row source. Each uses nonempty, sorted,
  // disjoint spans of uint32 indices; a span's exclusive end may be 2^32.
  // Over 20 changed indices yields one Invalidated event (index unused).
  // Otherwise a newly selected sole item yields Selected followed by Removed
  // only if exactly one item was removed. Other deltas list additions then
  // removals, each in ascending index order.
  std::vector<TrackSelectionEvent> trackSelectionNotifications(std::span<TrackSelectionSpan const> before,
                                                               std::span<TrackSelectionSpan const> after);
} // namespace ao::winui
