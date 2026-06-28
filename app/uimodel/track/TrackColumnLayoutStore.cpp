// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/Type.h>
#include <ao/rt/TrackField.h>
#include <ao/uimodel/track/TrackColumnLayoutStore.h>

#include <map>
#include <vector>

namespace ao::uimodel::track
{
  void TrackColumnLayoutStore::setListLayouts(std::map<ListId, std::vector<ColumnState>> const& layouts)
  {
    if (_listLayouts == layouts)
    {
      return;
    }

    _listLayouts = layouts;
    _changed.emit(kInvalidListId);
  }

  std::vector<ColumnState> const& TrackColumnLayoutStore::layoutForList(ListId listId) const noexcept
  {
    static std::vector<ColumnState> const kEmpty{};

    if (auto const it = _listLayouts.find(listId); it != _listLayouts.end())
    {
      return it->second;
    }

    return kEmpty;
  }

  void TrackColumnLayoutStore::updateLayout(ListId listId, std::vector<ColumnState> const& layout)
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

  void TrackColumnLayoutStore::setActiveListId(ListId listId)
  {
    if (_activeListId == listId)
    {
      return;
    }

    _activeListId = listId;
    _changed.emit(_activeListId);
  }

  std::vector<rt::TrackField> TrackColumnLayoutStore::activeFieldOrder() const
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
} // namespace ao::uimodel::track
