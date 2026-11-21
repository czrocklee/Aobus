// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/rt/TrackField.h>
#include <ao/uimodel/library/track/TrackAuthoring.h>

#include <cstdint>
#include <string>
#include <string_view>
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
    Date,
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
    std::string creditsLabel;
  };

  Result<TrackFieldEditValue> parseTrackPropertiesFieldInput(TrackPropertiesFormEditorKind kind, std::string_view text);
  /// Localized syntax hint for the constrained scalar editors (empty for ordinary text).
  std::string_view trackPropertiesInputHint(i18n::MessageCatalog const& textCatalog,
                                            TrackPropertiesFormEditorKind kind);
  TrackPropertiesFormSpec buildTrackPropertiesFormSpec(i18n::MessageCatalog const& textCatalog);
} // namespace ao::uimodel
