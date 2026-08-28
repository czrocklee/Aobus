// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/library/presentation/TrackColumnLayouts.h>

#include <ao/CoreIds.h>
#include <ao/rt/TrackField.h>

#include <algorithm>
#include <map>
#include <span>
#include <utility>
#include <vector>

namespace ao::uimodel
{
  void TrackColumnLayouts::restore(Snapshot layouts)
  {
    if (_listLayouts == layouts)
    {
      return;
    }

    _listLayouts = std::move(layouts);
    _changed.emit(kInvalidListId);
  }

  std::vector<TrackColumnState> const& TrackColumnLayouts::layoutForList(ListId listId) const noexcept
  {
    static std::vector<TrackColumnState> const kEmpty{};

    if (auto const it = _listLayouts.find(listId); it != _listLayouts.end())
    {
      return it->second;
    }

    return kEmpty;
  }

  void TrackColumnLayouts::updateLayout(ListId listId, std::vector<TrackColumnState> const& layout)
  {
    if (listId == kInvalidListId)
    {
      return;
    }

    if (_listLayouts[listId] == layout)
    {
      return;
    }

    _listLayouts[listId] = layout;
    _changed.emit(listId);
  }

  void TrackColumnLayouts::setActiveListId(ListId listId)
  {
    if (_activeListId == listId)
    {
      return;
    }

    _activeListId = listId;
  }

  std::vector<rt::TrackField> TrackColumnLayouts::activeFieldOrder() const
  {
    auto const& layout = layoutForList(_activeListId);
    auto order = std::vector<rt::TrackField>{};
    order.reserve(layout.size());

    for (auto const& col : layout)
    {
      order.push_back(col.field);
    }

    return order;
  }

  std::vector<rt::TrackField> visibleTrackFieldsInStoredOrder(std::span<rt::TrackField const> const visibleFields,
                                                              std::span<rt::TrackField const> const storedOrder)
  {
    auto ordered = std::vector<rt::TrackField>{};
    ordered.reserve(visibleFields.size());

    auto const appendIfVisible = [&ordered, visibleFields](rt::TrackField const field)
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
