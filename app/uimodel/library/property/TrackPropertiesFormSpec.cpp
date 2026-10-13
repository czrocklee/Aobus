// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/library/property/TrackPropertiesFormSpec.h>

#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/TrackField.h>
#include <ao/uimodel/library/presentation/TrackPresentationText.h>
#include <ao/uimodel/library/track/TrackAuthoring.h>

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
