// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>

#include <cstdint>
#include <limits>

namespace ao::rt
{
  inline constexpr auto kAllTracksListId = ListId{std::numeric_limits<std::uint32_t>::max()};

  /// Returns true for list IDs that do not reference a user-created list:
  /// kInvalidListId (no selection / root parent) and kAllTracksListId.
  constexpr bool isVirtualListId(ListId id)
  {
    return id == kInvalidListId || id == kAllTracksListId;
  }

  /// Maps a list's parentId to the source that list derives from.
  /// kInvalidListId is the All Tracks root; real IDs pass through.
  constexpr ListId resolveParentSourceId(ListId parentId)
  {
    return parentId == kInvalidListId ? kAllTracksListId : parentId;
  }
} // namespace ao::rt
