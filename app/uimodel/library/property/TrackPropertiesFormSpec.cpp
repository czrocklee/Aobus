// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/library/property/TrackPropertiesFormSpec.h>

#include <ao/Error.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/TrackField.h>
#include <ao/uimodel/library/presentation/TrackPresentationText.h>
#include <ao/uimodel/library/track/TrackAuthoring.h>

#include <string_view>

namespace ao::uimodel
{
  namespace
  {
    TrackPropertiesFormEditorKind editorKindFor(rt::TrackFieldDefinition const& def)
    {
      if (def.valueKind == rt::TrackFieldValueKind::Number)
      {
        return TrackPropertiesFormEditorKind::Number;
      }

      if (def.field == rt::TrackField::RecordingDate)
      {
        return TrackPropertiesFormEditorKind::Date;
      }

      return TrackPropertiesFormEditorKind::Text;
    }

    bool isEditableMetadataRow(rt::TrackFieldDefinition const& def)
    {
      return def.category == rt::TrackFieldCategory::Metadata && def.editable && def.field != rt::TrackField::Tags &&
             canWriteTrackFieldPatch(def.field);
    }

    bool isReadonlyPropertyRow(rt::TrackFieldDefinition const& def)
    {
      return def.category == rt::TrackFieldCategory::Technical && !def.synthetic;
    }
  } // namespace

  Result<TrackFieldEditValue> parseTrackPropertiesFieldInput(TrackPropertiesFormEditorKind const kind,
                                                             std::string_view const text)
  {
    switch (kind)
    {
      case TrackPropertiesFormEditorKind::Text: return parseTextEditValue(text);
      case TrackPropertiesFormEditorKind::Number: return parseUint16EditValue(text);
      case TrackPropertiesFormEditorKind::Date: return parseRecordingDateEditValue(text);
      case TrackPropertiesFormEditorKind::ReadonlyText:
        return makeError(Error::Code::InvalidInput, "A read-only property cannot be edited.");
    }

    return makeError(Error::Code::InvalidInput, "Unknown track property editor kind.");
  }

  std::string_view trackPropertiesInputHint(i18n::MessageCatalog const& textCatalog, TrackPropertiesFormEditorKind kind)
  {
    if (kind == TrackPropertiesFormEditorKind::Number)
    {
      return i18n::requiredText(textCatalog, i18n::MessageId::TrackNumberInvalid);
    }

    if (kind == TrackPropertiesFormEditorKind::Date)
    {
      return i18n::requiredText(textCatalog, i18n::MessageId::TrackRecordingDateInvalid);
    }

    return {};
  }

  TrackPropertiesFormSpec buildTrackPropertiesFormSpec(i18n::MessageCatalog const& textCatalog)
  {
    auto spec = TrackPropertiesFormSpec{
      .metadataRows = {},
      .propertyRows = {},
      .creditsLabel = std::string{i18n::requiredText(textCatalog, i18n::MessageId::TrackCreditsHeading)}};

    for (auto const& def : rt::trackFieldDefinitions())
    {
      if (isEditableMetadataRow(def))
      {
        spec.metadataRows.push_back(TrackPropertiesFormRow{
          .field = def.field,
          .label = std::string{trackFieldLabel(textCatalog, def.field)},
          .editorKind = editorKindFor(def),
        });
      }

      if (rt::creditKindForTrackField(def.field))
      {
        spec.metadataRows.push_back(TrackPropertiesFormRow{
          .field = def.field,
          .label = std::string{trackFieldLabel(textCatalog, def.field)},
          .editorKind = TrackPropertiesFormEditorKind::ReadonlyText,
        });
      }

      if (isReadonlyPropertyRow(def))
      {
        spec.propertyRows.push_back(TrackPropertiesFormRow{
          .field = def.field,
          .label = std::string{trackFieldLabel(textCatalog, def.field)},
          .editorKind = TrackPropertiesFormEditorKind::ReadonlyText,
        });
      }
    }

    return spec;
  }
} // namespace ao::uimodel
