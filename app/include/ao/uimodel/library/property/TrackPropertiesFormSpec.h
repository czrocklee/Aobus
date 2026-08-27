// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/rt/TrackField.h>

#include <cstdint>
#include <string>
#include <vector>

namespace ao::i18n
{
  class MessageCatalog;
}

namespace ao::uimodel
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
    std::string label;
    TrackPropertiesFormEditorKind editorKind = TrackPropertiesFormEditorKind::Text;

    bool operator==(TrackPropertiesFormRow const&) const = default;
  };

  struct TrackPropertiesFormSpec final
  {
    std::vector<TrackPropertiesFormRow> metadataRows;
    std::vector<TrackPropertiesFormRow> propertyRows;
  };

  TrackPropertiesFormSpec buildTrackPropertiesFormSpec(i18n::MessageCatalog const& textCatalog);
} // namespace ao::uimodel
