// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/library/presentation/TrackColumnLayoutPolicy.h>

#include <ao/rt/TrackField.h>
#include <ao/uimodel/library/presentation/TrackColumnLayoutStore.h>

#include <algorithm>
#include <span>
#include <vector>

namespace ao::uimodel
{
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

  std::vector<rt::TrackField> visibleTrackFieldsInStoredLayout(std::span<rt::TrackField const> const presentationFields,
                                                               std::span<TrackColumnState const> const storedLayout)
  {
    auto enabled = std::vector<rt::TrackField>{};
    enabled.reserve(presentationFields.size());

    for (auto const field : presentationFields)
    {
      auto const stored = std::ranges::find(storedLayout, field, &TrackColumnState::field);

      if (stored == storedLayout.end() || stored->visible)
      {
        enabled.push_back(field);
      }
    }

    auto order = std::vector<rt::TrackField>{};
    order.reserve(storedLayout.size());

    for (auto const& column : storedLayout)
    {
      order.push_back(column.field);
    }

    return visibleTrackFieldsInStoredOrder(enabled, order);
  }

  std::vector<TrackColumnState> mergeVisibleTrackColumnLayout(std::span<TrackColumnState const> const storedLayout,
                                                              std::span<TrackColumnState const> const visibleLayout)
  {
    auto merged = std::vector<TrackColumnState>{};
    merged.reserve(storedLayout.size() + visibleLayout.size());
    auto nextVisible = visibleLayout.begin();

    for (auto const& stored : storedLayout)
    {
      auto const participates = std::ranges::contains(visibleLayout, stored.field, &TrackColumnState::field);

      if (!participates)
      {
        merged.push_back(stored);
        continue;
      }

      if (nextVisible != visibleLayout.end())
      {
        merged.push_back(*nextVisible);
        ++nextVisible;
      }
    }

    merged.insert(merged.end(), nextVisible, visibleLayout.end());
    return merged;
  }
} // namespace ao::uimodel
