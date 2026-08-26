// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace ao::uimodel
{
  struct TrackDisplaySection final
  {
    std::size_t start = 0;
    std::size_t count = 0;

    friend bool operator==(TrackDisplaySection const&, TrackDisplaySection const&) = default;
  };

  enum class TrackDisplayItemKind : std::uint8_t
  {
    GroupHeader,
    TrackRow,
  };

  struct TrackDisplayItem final
  {
    TrackDisplayItemKind kind = TrackDisplayItemKind::TrackRow;
    std::size_t sourceIndex = 0;
    std::size_t groupIndex = 0;

    friend bool operator==(TrackDisplayItem const&, TrackDisplayItem const&) = default;
  };

  /**
   * Maps a virtualized display index to either a group header or projection row.
   *
   * Sections must be ordered, contiguous, non-empty, and cover every source row.
   * Passing no sections represents an ungrouped projection.
   */
  class TrackDisplayIndex final
  {
  public:
    bool reset(std::size_t rowCount, std::span<TrackDisplaySection const> sections);
    void clear() noexcept;

    std::size_t rowCount() const noexcept { return _rowCount; }
    std::size_t groupCount() const noexcept { return _sections.size(); }
    std::size_t displayCount() const noexcept;
    std::optional<TrackDisplayItem> itemAt(std::size_t displayIndex) const noexcept;
    std::optional<std::size_t> displayIndexOfSourceRow(std::size_t sourceIndex) const noexcept;

  private:
    std::size_t _rowCount = 0;
    std::vector<TrackDisplaySection> _sections;
  };
} // namespace ao::uimodel
