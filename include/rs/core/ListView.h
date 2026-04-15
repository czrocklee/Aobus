// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <rs/core/ListLayout.h>
#include <rs/utility/ByteView.h>

#include <cstdint>
#include <ranges>
#include <span>
#include <string_view>

namespace rs::core
{
  /**
   * ListView - Safe accessor for list data stored in binary format.
   * Reads fields directly from payload without storing header.
   *
   * Smart List Inheritance:
   * - Smart lists have a non-empty filter() and define membership as:
   *   effective_membership = parent_membership AND filter()
   * - For root lists (parentId == 0), parent_membership = all tracks
   * - For nested lists, membership chains through ancestor filters
   * - The local filter() is stored as-is (not combined with parent)
   */
  class ListView final
  {
  public:
    explicit ListView(std::span<std::byte const> data);

    std::string_view name() const;
    std::string_view description() const;

    /**
     * Local filter expression for smart lists.
     * For inherited filtering, effective filter is computed at runtime
     * as parent_membership AND this filter.
     */
    std::string_view filter() const;

    /** Parent list ID (0 = All Tracks / root) */
    ListId parentId() const noexcept;

    /** True if this list's parent is All Tracks (the root) */
    bool isRootParent() const noexcept;

    /** True if this is a smart list (has a local filter) */
    bool isSmart() const noexcept { return !filter().empty(); }

    /**
     * TrackProxy - Iterator and index access to track IDs in the list.
     * For smart lists, this returns the stored track IDs which should be ignored;
     * use FilteredTrackIdList + SmartListEngine for dynamic membership instead.
     */
    class TrackProxy : public std::ranges::view_interface<TrackProxy>
    {
    public:
      TrackProxy(std::span<TrackId const> trackIds);

      TrackId at(std::size_t index) const;
      TrackId operator[](std::size_t index) const { return at(index); }

      TrackId const* begin() const { return _trackIds.data(); }
      TrackId const* end() const { return _trackIds.data() + _trackIds.size(); }
      std::size_t size() const { return _trackIds.size(); }

    private:
      std::span<TrackId const> _trackIds;
    };

    TrackProxy tracks() const;

  private:
    ListHeader const* header() const { return utility::layout::view<ListHeader>(_payload); }
    std::string_view getString(std::uint16_t offset, std::uint16_t length) const;

    std::span<std::byte const> _payload;
  };

} // namespace rs::core
