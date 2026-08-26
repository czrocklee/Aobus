// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/library/track/TrackDisplayIndex.h>

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <optional>
#include <span>

namespace ao::uimodel
{
  bool TrackDisplayIndex::reset(std::size_t const rowCount, std::span<TrackDisplaySection const> const sections)
  {
    if (!sections.empty())
    {
      std::size_t expectedStart = 0;

      for (auto const& section : sections)
      {
        if (section.count == 0 || section.start != expectedStart || section.count > rowCount - expectedStart)
        {
          return false;
        }

        expectedStart += section.count;
      }

      if (expectedStart != rowCount)
      {
        return false;
      }
    }

    _rowCount = rowCount;
    _sections.assign(sections.begin(), sections.end());
    return true;
  }

  void TrackDisplayIndex::clear() noexcept
  {
    _rowCount = 0;
    _sections.clear();
  }

  std::size_t TrackDisplayIndex::displayCount() const noexcept
  {
    return _rowCount + _sections.size();
  }

  std::optional<TrackDisplayItem> TrackDisplayIndex::itemAt(std::size_t const displayIndex) const noexcept
  {
    if (displayIndex >= displayCount())
    {
      return std::nullopt;
    }

    if (_sections.empty())
    {
      return TrackDisplayItem{.kind = TrackDisplayItemKind::TrackRow, .sourceIndex = displayIndex};
    }

    std::size_t first = 0;
    auto last = _sections.size();

    while (first < last)
    {
      auto const middle = first + ((last - first) / 2);

      if (auto const headerIndex = _sections[middle].start + middle; headerIndex <= displayIndex)
      {
        first = middle + 1;
      }
      else
      {
        last = middle;
      }
    }

    auto const groupIndex = first - 1;
    auto const& section = _sections[groupIndex];
    auto const headerIndex = section.start + groupIndex;

    if (displayIndex == headerIndex)
    {
      return TrackDisplayItem{
        .kind = TrackDisplayItemKind::GroupHeader,
        .sourceIndex = section.start,
        .groupIndex = groupIndex,
      };
    }

    return TrackDisplayItem{
      .kind = TrackDisplayItemKind::TrackRow,
      .sourceIndex = section.start + (displayIndex - headerIndex - 1),
      .groupIndex = groupIndex,
    };
  }

  std::optional<std::size_t> TrackDisplayIndex::displayIndexOfSourceRow(std::size_t const sourceIndex) const noexcept
  {
    if (sourceIndex >= _rowCount)
    {
      return std::nullopt;
    }

    if (_sections.empty())
    {
      return sourceIndex;
    }

    auto const next = std::ranges::upper_bound(_sections, sourceIndex, {}, &TrackDisplaySection::start);
    auto const groupIndex = static_cast<std::size_t>(std::distance(_sections.begin(), next) - 1);
    return sourceIndex + groupIndex + 1;
  }
} // namespace ao::uimodel
