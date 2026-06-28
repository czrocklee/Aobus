// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Type.h>
#include <ao/rt/CorePrimitives.h>
#include <ao/rt/TrackField.h>

#include <cstdint>
#include <map>
#include <vector>

namespace ao::uimodel::track
{
  struct ColumnState final
  {
    rt::TrackField field = rt::TrackField::Title;
    std::int32_t width = -1;

    bool operator==(ColumnState const&) const = default;
  };

  struct TrackColumnLayoutState final
  {
    std::map<ListId, std::vector<ColumnState>> listLayouts;
  };

  class TrackColumnLayoutStore final
  {
  public:
    std::map<ListId, std::vector<ColumnState>> const& listLayouts() const noexcept { return _listLayouts; }
    void setListLayouts(std::map<ListId, std::vector<ColumnState>> const& layouts);

    std::vector<ColumnState> const& layoutForList(ListId listId) const noexcept;
    void updateLayout(ListId listId, std::vector<ColumnState> const& layout);

    void setActiveListId(ListId listId);
    ListId activeListId() const noexcept { return _activeListId; }
    std::vector<rt::TrackField> activeFieldOrder() const;

    rt::Signal<ListId>& signalChanged() noexcept { return _changed; }

  private:
    ListId _activeListId = kInvalidListId;
    std::map<ListId, std::vector<ColumnState>> _listLayouts{};
    rt::Signal<ListId> _changed;
  };
} // namespace ao::uimodel::track
