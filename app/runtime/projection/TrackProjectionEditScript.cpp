// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/rt/TrackEditScript.h>
#include <ao/rt/projection/TrackListProjection.h>
#include <ao/rt/projection/TrackProjectionEditScript.h>

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <ranges>
#include <type_traits>
#include <variant>
#include <vector>

namespace ao::rt
{
  namespace
  {
    void sortUnique(std::vector<std::size_t>& rowIndices)
    {
      std::ranges::sort(rowIndices);
      rowIndices.erase(std::ranges::unique(rowIndices).begin(), rowIndices.end());
    }

    template<typename Range>
    void appendAscending(TrackListProjectionDeltaBatch& batch, std::vector<std::size_t>& rowIndices)
    {
      if (rowIndices.empty())
      {
        return;
      }

      sortUnique(rowIndices);
      auto start = rowIndices.front();
      auto previous = start;

      for (auto const rowIndex : rowIndices | std::views::drop(1))
      {
        if (rowIndex == previous + 1)
        {
          previous = rowIndex;
          continue;
        }

        batch.deltas.push_back(Range{TrackRowRange{.start = start, .count = previous - start + 1}});
        start = rowIndex;
        previous = rowIndex;
      }

      batch.deltas.push_back(Range{TrackRowRange{.start = start, .count = previous - start + 1}});
    }
  } // namespace

  TrackListProjectionDeltaBatch eraseTrackIds(delta::RegularTrackEditScript const& script)
  {
    auto batch = TrackListProjectionDeltaBatch{};
    batch.deltas.reserve(script.edits.size());

    for (auto const& edit : script.edits)
    {
      std::visit(
        [&batch](auto const& range)
        {
          using Range = std::remove_cvref_t<decltype(range)>;
          auto const rowRange = TrackRowRange{.start = range.start, .count = range.trackIds.size()};

          if constexpr (std::same_as<Range, delta::InsertRange>)
          {
            batch.deltas.push_back(ProjectionInsertRange{rowRange});
          }
          else if constexpr (std::same_as<Range, delta::RemoveRange>)
          {
            batch.deltas.push_back(ProjectionRemoveRange{rowRange});
          }
          else
          {
            batch.deltas.push_back(ProjectionUpdateRange{rowRange});
          }
        },
        edit);
    }

    return batch;
  }

  void appendProjectionInsertRanges(TrackListProjectionDeltaBatch& batch, std::vector<std::size_t>& rowIndices)
  {
    appendAscending<ProjectionInsertRange>(batch, rowIndices);
  }

  void appendProjectionRemoveRanges(TrackListProjectionDeltaBatch& batch, std::vector<std::size_t>& rowIndices)
  {
    if (rowIndices.empty())
    {
      return;
    }

    sortUnique(rowIndices);
    std::ranges::reverse(rowIndices);
    auto high = rowIndices.front();
    auto low = high;

    for (auto const rowIndex : rowIndices | std::views::drop(1))
    {
      if (rowIndex + 1 == low)
      {
        low = rowIndex;
        continue;
      }

      batch.deltas.push_back(ProjectionRemoveRange{TrackRowRange{.start = low, .count = high - low + 1}});
      high = rowIndex;
      low = rowIndex;
    }

    batch.deltas.push_back(ProjectionRemoveRange{TrackRowRange{.start = low, .count = high - low + 1}});
  }

  void appendProjectionUpdateRanges(TrackListProjectionDeltaBatch& batch, std::vector<std::size_t>& rowIndices)
  {
    appendAscending<ProjectionUpdateRange>(batch, rowIndices);
  }

  namespace
  {
    constexpr delta::RangeEditKind rangeEditKindOf(ProjectionRemoveRange const& /*range*/) noexcept
    {
      return delta::RangeEditKind::Remove;
    }

    constexpr delta::RangeEditKind rangeEditKindOf(ProjectionInsertRange const& /*range*/) noexcept
    {
      return delta::RangeEditKind::Insert;
    }

    constexpr delta::RangeEditKind rangeEditKindOf(ProjectionUpdateRange const& /*range*/) noexcept
    {
      return delta::RangeEditKind::Update;
    }
  } // namespace

  bool validateTrackListProjectionDeltaBatch(TrackListProjectionDeltaBatch const& batch, std::size_t initialSize)
  {
    if (batch.deltas.empty())
    {
      return false;
    }

    if (std::holds_alternative<ProjectionReset>(batch.deltas.front()) ||
        std::holds_alternative<ProjectionSourceInvalidated>(batch.deltas.front()))
    {
      return batch.deltas.size() == 1;
    }

    auto validator = delta::RangeEditValidator{initialSize};

    for (auto const& edit : batch.deltas)
    {
      auto const accepted = std::visit(
        [&validator](auto const& range)
        {
          using Range = std::remove_cvref_t<decltype(range)>;

          if constexpr (std::same_as<Range, ProjectionReset> || std::same_as<Range, ProjectionSourceInvalidated>)
          {
            // A reset or invalidation is only ever valid on its own, and that
            // case returned above.
            return false;
          }
          else
          {
            return validator.accept(rangeEditKindOf(range), range.range.start, range.range.count);
          }
        },
        edit);

      if (!accepted)
      {
        return false;
      }
    }

    return true;
  }
} // namespace ao::rt
