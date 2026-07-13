// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>

#include <gsl-lite/gsl-lite.hpp>

#include <cstddef>
#include <cstdint>
#include <ranges>
#include <span>
#include <string_view>

namespace ao::library
{
  struct ListHeader;

  /**
   * ListView - Read-only view over a serialized list record.
   *
   * Record contract (see doc/reference/library/storage/database.md):
   * records are produced by ListBuilder and validated at write time, so the
   * constructor runs a single O(1) structural gate (header fits, track-id
   * array and string extents stay inside the record). A record that fails
   * the gate is a poisoned view: isValid() reports false and every accessor
   * returns an empty/zero value. Accessors never throw and never read out
   * of bounds.
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
    explicit ListView(std::span<std::byte const> data) noexcept;

    /** True when the record passed its structural gate. */
    bool isValid() const noexcept { return _header != nullptr; }

    std::string_view name() const noexcept;
    std::string_view description() const noexcept;

    /**
     * Local filter expression for smart lists.
     * For inherited filtering, effective filter is computed at runtime
     * as parent_membership AND this filter.
     */
    std::string_view filter() const noexcept;

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
      TrackProxy() = default;

      explicit TrackProxy(std::span<TrackId const> trackIds) noexcept
        : _trackIds{trackIds}
      {
      }

      TrackId at(std::size_t index) const noexcept
      {
        gsl_Expects(index < _trackIds.size());
        return _trackIds[index];
      }

      TrackId operator[](std::size_t index) const noexcept { return at(index); }

      TrackId const* begin() const noexcept { return _trackIds.data(); }
      TrackId const* end() const noexcept { return _trackIds.data() + _trackIds.size(); }
      // These O(1) members intentionally refine view_interface's CRTP fallback.
      // NOLINTNEXTLINE(bugprone-derived-method-shadowing-base-method)
      bool empty() const noexcept { return _trackIds.empty(); }
      // NOLINTNEXTLINE(bugprone-derived-method-shadowing-base-method)
      std::size_t size() const noexcept { return _trackIds.size(); }

    private:
      std::span<TrackId const> _trackIds{};
    };

    TrackProxy tracks() const noexcept;

    std::span<std::byte const> rawData() const noexcept { return _payload; }

  private:
    std::string_view stringAt(std::uint16_t offset, std::uint16_t length) const noexcept;

    std::span<std::byte const> _payload;
    ListHeader const* _header = nullptr;
  };
} // namespace ao::library
