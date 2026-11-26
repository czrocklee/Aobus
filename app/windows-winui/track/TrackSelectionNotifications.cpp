// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/winui/track/TrackSelectionNotifications.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ao::winui
{
  namespace
  {
    constexpr std::size_t kItemEventLimit = 20;

    std::uint64_t spanEnd(TrackSelectionSpan const span) noexcept
    {
      return std::uint64_t{span.first} + span.length;
    }

    bool tryAppendEvents(std::vector<TrackSelectionEvent>& events,
                         TrackSelectionEventKind const kind,
                         std::uint64_t const first,
                         std::uint64_t const end)
    {
      if (end - first > kItemEventLimit - events.size())
      {
        return false;
      }

      for (auto index = first; index < end; ++index)
      {
        events.push_back({.kind = kind, .index = static_cast<std::uint32_t>(index)});
      }

      return true;
    }

    bool tryAppendDifference(std::span<TrackSelectionSpan const> const candidates,
                             std::span<TrackSelectionSpan const> excluded,
                             TrackSelectionEventKind const kind,
                             std::vector<TrackSelectionEvent>& events)
    {
      for (auto const candidate : candidates)
      {
        std::uint64_t first = candidate.first;
        auto const end = spanEnd(candidate);

        while (!excluded.empty() && excluded.front().first < end)
        {
          auto const other = excluded.front();

          if (first < other.first && !tryAppendEvents(events, kind, first, other.first))
          {
            return false;
          }

          first = std::max(first, spanEnd(other));

          if (first >= end)
          {
            // This span may also cover later candidates.
            break;
          }

          excluded = excluded.subspan(1);
        }

        if (first < end && !tryAppendEvents(events, kind, first, end))
        {
          return false;
        }
      }

      return true;
    }
  } // namespace

  std::vector<TrackSelectionEvent> trackSelectionNotifications(std::span<TrackSelectionSpan const> const before,
                                                               std::span<TrackSelectionSpan const> const after)
  {
    auto events = std::vector<TrackSelectionEvent>{};
    auto const additionKind = after.size() == 1 && after.front().length == 1 ? TrackSelectionEventKind::Selected
                                                                             : TrackSelectionEventKind::Added;

    if (!tryAppendDifference(after, before, additionKind, events) ||
        !tryAppendDifference(before, after, TrackSelectionEventKind::Removed, events))
    {
      return {{.kind = TrackSelectionEventKind::Invalidated, .index = 0}};
    }

    if (events.size() > 2 && events.front().kind == TrackSelectionEventKind::Selected)
    {
      // A new sole selection replaces the old selection as a whole. Report a
      // specific old item as removed only when there was exactly one.
      events.resize(1);
    }

    return events;
  }
} // namespace ao::winui
