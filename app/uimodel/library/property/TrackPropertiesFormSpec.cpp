// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/TrackField.h>
#include <ao/uimodel/field/TrackFieldEditPolicy.h>
#include <ao/uimodel/library/property/TrackPropertiesFormSpec.h>

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

  TrackPropertiesFormSpec buildTrackPropertiesFormSpec()
  {
    auto spec = TrackPropertiesFormSpec{};

    for (auto const& def : rt::trackFieldDefinitions())
    {
      if (isEditableMetadataRow(def))
      {
        spec.metadataRows.push_back(TrackPropertiesFormRow{
          .field = def.field,
          .label = def.label,
          .editorKind = editorKindFor(def),
        });
      }

      if (isReadonlyPropertyRow(def))
      {
        spec.propertyRows.push_back(TrackPropertiesFormRow{
          .field = def.field,
          .label = def.label,
          .editorKind = TrackPropertiesFormEditorKind::ReadonlyText,
        });
      }
    }

    return spec;
  }
} // namespace ao::uimodel
