// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/rt/StateTypes.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackFieldValue.h>
#include <ao/uimodel/track/TrackFieldFormatter.h>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace ao::gtk
{
  class TrackRowObject;
  class TrackRowCache;

  using TrackFieldRawValue = rt::TrackFieldRawValue;

  using TrackFieldEditValue = uimodel::track::TrackFieldEditValue;

  struct TrackFieldEditContext final
  {
    rt::MetadataPatch& patch;
    TrackFieldEditValue const& value;
  };

  constexpr auto kTagsCellCssClass = "ao-track-tags-cell";

  using TrackRowTextReader = std::string (*)(TrackRowObject const&, TrackRowCache const&);
  using TrackFieldFormatter = std::string (*)(TrackFieldRawValue const&);
  using TrackInlineEditParser = Result<TrackFieldEditValue> (*)(std::string_view);
  using TrackRowEditReader = TrackFieldEditValue (*)(TrackRowObject const&, rt::TrackField);
  using TrackRowEditApplier = bool (*)(TrackRowObject&, TrackFieldEditValue const&, rt::TrackField);
  using TrackFieldPatchWriter = void (*)(TrackFieldEditContext const&);

  struct TrackFieldUiDefinition final
  {
    rt::TrackField field = rt::TrackField::Title;

    TrackRowTextReader readRowText = nullptr;
    TrackFieldFormatter formatValue = nullptr;
    TrackInlineEditParser parseInlineEdit = nullptr;
    TrackRowEditReader readRowEditValue = nullptr;
    TrackRowEditApplier applyRowEditValue = nullptr;
    TrackFieldPatchWriter writePatch = nullptr;
  };

  bool canInlineEdit(TrackFieldUiDefinition const& def);

  std::span<TrackFieldUiDefinition const> trackFieldUiDefinitions();
  TrackFieldUiDefinition const* trackFieldUiDefinition(rt::TrackField field);

  std::int32_t defaultWidthForField(rt::TrackField field);
  bool fieldIsVisibleByDefault(rt::TrackField field);
  std::string_view fieldColumnTitle(rt::TrackField field);

  std::optional<rt::TrackField> redundantFieldToColumn(rt::TrackSortField field);
} // namespace ao::gtk
