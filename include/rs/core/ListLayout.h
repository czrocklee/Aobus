// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <rs/core/Type.h>

#include <cstdint>

namespace rs::core
{

  /**
   * ListHeader - POD struct for binary list storage.
   * Layout follows TrackLayout pattern with 4-byte alignment.
   * trackIds array starts immediately after header at sizeof(ListHeader).
   * Smart vs manual is determined by filter: if filter is empty -> manual, else -> smart.
   *
   * Layout:
   *   ┌─────────────────────────────────────┐  ← header begin
   *   │        ListHeader (16B)             │
   *   │  trackIdsCount (4B)                 │
   *   │  nameOffset, nameLen (4B)           │
   *   │  descOffset, descLen (4B)           │
   *   │  filterOffset, filterLen (4B)       │
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
    std::uint16_t nameOffset;   // Byte offset from END of trackIds to name string
    std::uint16_t nameLen;      // Length of name string
    std::uint16_t descOffset;   // Byte offset from END of trackIds to description string
    std::uint16_t descLen;      // Length of description string
    std::uint16_t filterOffset; // Byte offset from END of trackIds to filter expression
    std::uint16_t filterLen;    // Length of filter expression string
  };

  // Binary layout constants
  constexpr std::size_t kListHeaderSize = 16;
  constexpr std::size_t kListHeaderAlignment = 4;

  static_assert(sizeof(ListHeader) == kListHeaderSize, "ListHeader must be exactly 16 bytes");
  static_assert(alignof(ListHeader) == kListHeaderAlignment, "ListHeader must have 4-byte alignment");

} // namespace rs::core
