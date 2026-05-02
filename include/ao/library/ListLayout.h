// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <ao/Type.h>

#include <cstdint>

namespace ao::library
{
  /**
   * ListHeader - POD struct for binary list storage.
   * Layout follows TrackLayout pattern with 4-byte alignment.
   * trackIds array starts immediately after header at sizeof(ListHeader).
   *
   * Smart vs Manual List Determination:
   * - A list is "smart" if filter() is non-empty, otherwise it's "manual"
   * - Manual lists store explicit track IDs for membership
   * - Smart lists ignore stored trackIds and compute membership dynamically
   *
   * Inherited Filtering Semantics:
   * - Each smart list has a parentId (0 = All Tracks, the root)
   * - A child smart list's effective membership = parent_membership AND local_filter
   * - This creates a chain: grandparent AND parent_filter AND child_filter AND ...
   * - The local filter is stored in filter() - it is NOT pre-combined with parent
   *
   * Layout:
   *   ┌─────────────────────────────────────┐  ← header begin
   *   │        ListHeader (20B)             │
   *   │  trackIdsCount (4B)                 │
   *   │  nameOffset, nameLen (4B)           │
   *   │  descOffset, descLen (4B)           │
   *   │  filterOffset, filterLen (4B)       │
   *   │  parentId (4B)                      │
   *   ├─────────────────────────────────────┤  ← trackIds begin = sizeof(ListHeader)
   *   │  track ID 1 (4B)                    │
   *   │  track ID 2 (4B)                    │
   *   │  ...                                │
   *   ├─────────────────────────────────────┤  ← name = sizeof(ListHeader) + nameOffset
   *   │  name string...                      │
   *   ├─────────────────────────────────────┤  ← desc = sizeof(ListHeader) + descOffset
   *   │  description string...               │
   *   ├─────────────────────────────────────┤  ← filter = sizeof(ListHeader) + filterOffset
   *   │  filter expression string...          │
   *   └─────────────────────────────────────┘
   */
  struct ListHeader final
  {
    // 4-byte section
    std::uint32_t trackIdsCount; // Number of track IDs (trackIds always start at sizeof(header))

    // 2-byte section
    std::uint16_t nameOffset;   // Byte offset from the track-id region start to the name string
    std::uint16_t nameLen;      // Length of name string
    std::uint16_t descOffset;   // Byte offset from the track-id region start to the description string
    std::uint16_t descLen;      // Length of description string
    std::uint16_t filterOffset; // Byte offset from the track-id region start to the filter expression
    std::uint16_t filterLen;    // Length of filter expression string
    std::uint32_t parentId;     // Parent list id, 0 = All Tracks
  };

  // Binary layout constants
  constexpr std::size_t kListHeaderSize = 20;
  constexpr std::size_t kListHeaderAlignment = 4;

  static_assert(sizeof(ListHeader) == kListHeaderSize, "ListHeader must be exactly 20 bytes");
  static_assert(alignof(ListHeader) == kListHeaderAlignment, "ListHeader must have 4-byte alignment");
} // namespace ao::library
