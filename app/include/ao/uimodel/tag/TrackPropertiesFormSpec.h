// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/rt/TrackField.h>

#include <cstdint>
#include <string_view>
#include <vector>

namespace ao::uimodel::tag
{
  enum class TrackPropertiesFormEditorKind : std::uint8_t
  {
    Text,
    Number,
    ReadonlyText,
  };

  struct TrackPropertiesFormRow final
  {
    rt::TrackField field = rt::TrackField::Title;
    std::string_view label;
    TrackPropertiesFormEditorKind editorKind = TrackPropertiesFormEditorKind::Text;

    bool operator==(TrackPropertiesFormRow const&) const = default;
  };

  struct TrackPropertiesFormSpec final
  {
    std::vector<TrackPropertiesFormRow> metadataRows;
    std::vector<TrackPropertiesFormRow> propertyRows;
  };

  TrackPropertiesFormSpec buildTrackPropertiesFormSpec();
} // namespace ao::uimodel::tag
