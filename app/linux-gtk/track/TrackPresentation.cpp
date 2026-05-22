// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackPresentation.h"

#include "runtime/TrackField.h"
#include "track/TrackFieldUi.h"

#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

namespace ao::gtk
{
  std::int32_t defaultWidthForField(rt::TrackField field)
  {
    if (auto const* uiDef = trackFieldUiDefinition(field); uiDef != nullptr)
    {
      return uiDef->defaultColumnWidth;
    }

    return -1;
  }

  bool fieldIsExpanding(rt::TrackField field)
  {
    auto const* uiDef = trackFieldUiDefinition(field);

    return uiDef != nullptr && uiDef->columnExpands;
  }

  bool fieldIsVisibleByDefault(rt::TrackField field)
  {
    auto const* uiDef = trackFieldUiDefinition(field);

    return uiDef != nullptr && uiDef->columnVisibleByDefault;
  }

  std::string_view fieldColumnTitle(rt::TrackField field)
  {
    if (auto const* rtDef = rt::trackFieldDefinition(field); rtDef != nullptr)
    {
      return rtDef->label;
    }

    return {};
  }

  std::optional<rt::TrackField> redundantFieldToColumn(rt::TrackSortField sortField)
  {
    for (auto const& def : rt::trackFieldDefinitions())
    {
      if (def.optSortField == sortField && def.groupable)
      {
        return def.field;
      }
    }

    return std::nullopt;
  }

  TrackColumnLayoutModel::TrackColumnLayoutModel(TrackColumnViewState state)
    : _state{std::move(state)}
  {
  }

  void TrackColumnLayoutModel::setState(TrackColumnViewState const& state)
  {
    if (_state == state)
    {
      return;
    }

    _state = state;
    _changed.emit();
  }

  void TrackColumnLayoutModel::reset()
  {
    setState(TrackColumnViewState{});
  }
} // namespace ao::gtk
