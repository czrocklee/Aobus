// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace ao::library
{
  // The highest storable List id. UINT32_MAX is never stored, so frontends can
  // use it to name a virtual List without colliding with a library List.
  constexpr std::uint32_t kMaxListId = std::numeric_limits<std::uint32_t>::max() - 1U;

  /**
   * Binary List record header.
   *
   * The variable region uses one canonical packing:
   * orderTrackIds, name, description, filter, then zero padding to four-byte
   * alignment. Byte offsets are derived from the count and preceding lengths,
   * so malformed overlapping or non-canonical slices are not representable.
   *
   * Layout:
   *   ┌─────────────────────────────────────┐  ← header begin
   *   │        ListHeader (20B)             │
   *   │  orderTrackIdCount (4B)             │
   *   │  nameLength (4B)                    │
   *   │  descLength (4B)                    │
   *   │  filterLength (4B)                  │
   *   │  parentId (4B)                      │
   *   ├─────────────────────────────────────┤  ← variable region begin
   *   │  order track ID 1 (4B)              │
   *   │  order track ID 2 (4B)              │
   *   │  ...                                │
   *   ├─────────────────────────────────────┤  ← derived name offset
   *   │  name string...                     │
   *   ├─────────────────────────────────────┤  ← derived description offset
   *   │  description string...              │
   *   ├─────────────────────────────────────┤  ← derived filter offset
   *   │  filter expression string...        │
   *   ├─────────────────────────────────────┤
   *   │  zero padding (0..3B)               │
   *   └─────────────────────────────────────┘
   */
  struct ListHeader final
  {
    std::uint32_t orderTrackIdCount = 0;
    std::uint32_t nameLength = 0;
    std::uint32_t descLength = 0;
    std::uint32_t filterLength = 0;
    std::uint32_t parentId = 0;
  };

  constexpr std::size_t kListHeaderSize = 20;
  constexpr std::size_t kListHeaderAlignment = 4;
  constexpr std::size_t kListFilterLengthOffset = 12;

  static_assert(sizeof(ListHeader) == kListHeaderSize, "ListHeader must be exactly 20 bytes");
  static_assert(alignof(ListHeader) == kListHeaderAlignment, "ListHeader must have 4-byte alignment");
  static_assert(offsetof(ListHeader, orderTrackIdCount) == 0);
  static_assert(offsetof(ListHeader, nameLength) == 4);
  static_assert(offsetof(ListHeader, descLength) == 8);
  static_assert(offsetof(ListHeader, filterLength) == kListFilterLengthOffset);
  static_assert(offsetof(ListHeader, parentId) == 16);
  static_assert(std::has_unique_object_representations_v<ListHeader>, "ListHeader must have no padding");
} // namespace ao::library
