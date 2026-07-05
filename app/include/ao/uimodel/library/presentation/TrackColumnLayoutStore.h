// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/rt/CorePrimitives.h>
#include <ao/rt/TrackField.h>

#include <cstdint>
#include <map>
#include <vector>

namespace ao::uimodel
{
  struct TrackColumnState final
  {
    rt::TrackField field = rt::TrackField::Title;
    std::int32_t width = -1;
    double weight = -1.0;

    bool operator==(TrackColumnState const&) const = default;
  };

  struct TrackColumnLayoutState final
  {
    std::map<ListId, std::vector<TrackColumnState>> listLayouts;
  };

  class TrackColumnLayoutStore final
  {
  public:
    std::map<ListId, std::vector<TrackColumnState>> const& listLayouts() const noexcept { return _listLayouts; }
    void setListLayouts(std::map<ListId, std::vector<TrackColumnState>> const& layouts);

    std::vector<TrackColumnState> const& layoutForList(ListId listId) const noexcept;
    void updateLayout(ListId listId, std::vector<TrackColumnState> const& layout);

    void setActiveListId(ListId listId);
    ListId activeListId() const noexcept { return _activeListId; }
    std::vector<rt::TrackField> activeFieldOrder() const;

    rt::Signal<ListId>& signalChanged() noexcept { return _changed; }

  private:
    ListId _activeListId = kInvalidListId;
    std::map<ListId, std::vector<TrackColumnState>> _listLayouts{};
    rt::Signal<ListId> _changed;
  };
} // namespace ao::uimodel
