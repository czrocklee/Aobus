// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/TrackField.h>
#include <ao/uimodel/library/presentation/TrackColumnLayoutPolicy.h>
#include <ao/uimodel/library/presentation/TrackFieldPresentationPolicy.h>

#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>

namespace ao::uimodel
{
  rt::TrackField expandingTrackColumn(std::span<rt::TrackField const> visibleFields)
  {
    if (std::ranges::contains(visibleFields, rt::TrackField::Title))
    {
      return rt::TrackField::Title;
    }

    if (std::ranges::contains(visibleFields, rt::TrackField::Tags))
    {
      return rt::TrackField::Tags;
    }

    if (!visibleFields.empty())
    {
      return visibleFields.front();
    }

    return rt::TrackField::Title;
  }

  std::vector<rt::TrackField> visibleTrackFieldsInStoredOrder(std::span<rt::TrackField const> visibleFields,
                                                              std::span<rt::TrackField const> storedOrder)
  {
    auto ordered = std::vector<rt::TrackField>{};
    ordered.reserve(visibleFields.size());

    auto const appendIfVisible = [&ordered, visibleFields](rt::TrackField field)
    {
      if (!std::ranges::contains(visibleFields, field) || std::ranges::contains(ordered, field))
      {
        return;
      }

      ordered.push_back(field);
    };

    for (auto const field : storedOrder)
    {
      appendIfVisible(field);
    }

    for (auto const field : visibleFields)
    {
      appendIfVisible(field);
    }

    return ordered;
  }

  std::int32_t effectiveTrackFieldColumnWidth(rt::TrackField field, std::int32_t storedWidth)
  {
    return storedWidth > 0 ? storedWidth : defaultTrackFieldColumnWidth(field);
  }
} // namespace ao::uimodel
