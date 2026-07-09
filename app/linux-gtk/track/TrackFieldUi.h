// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/rt/TrackField.h>
#include <ao/uimodel/field/TrackFieldEditCodec.h>

#include <span>
#include <string>
#include <string_view>

namespace ao::gtk
{
  class TrackRowObject;
  class TrackRowCache;

  using TrackFieldEditValue = uimodel::TrackFieldEditValue;

  constexpr auto kTagsCellCssClass = "ao-track-tags-cell";

  using TrackRowTextReader = std::string (*)(TrackRowObject const&, TrackRowCache const&);
  using TrackInlineEditParser = Result<TrackFieldEditValue> (*)(std::string_view);
  using TrackRowEditReader = TrackFieldEditValue (*)(TrackRowObject const&, rt::TrackField);
  using TrackRowEditWriter = bool (*)(TrackRowObject&, TrackFieldEditValue const&, rt::TrackField);

  struct TrackFieldUiDefinition final
  {
    rt::TrackField field = rt::TrackField::Title;

    TrackRowTextReader readRowText = nullptr;
    TrackInlineEditParser parseInlineEdit = nullptr;
    TrackRowEditReader readRowEditValue = nullptr;
    TrackRowEditWriter applyRowEditValue = nullptr;
  };

  bool canInlineEdit(TrackFieldUiDefinition const& def);

  std::span<TrackFieldUiDefinition const> trackFieldUiDefinitions();
  TrackFieldUiDefinition const* trackFieldUiDefinition(rt::TrackField field);
} // namespace ao::gtk
