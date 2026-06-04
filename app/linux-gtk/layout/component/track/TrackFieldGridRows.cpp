// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "TrackFieldGridRows.h"

#include "gtkmm/enums.h"
#include "track/TrackFieldUi.h"
#include <ao/rt/TrackField.h>

#include <cstdint>
#include <string>
#include <utility>

namespace ao::gtk::layout::track_field_grid
{
  namespace
  {
    bool isFieldEditable(rt::TrackField field)
    {
      auto const* uiDef = trackFieldUiDefinition(field);
      return uiDef != nullptr && uiDef->parseInlineEdit != nullptr && uiDef->writePatch != nullptr;
    }

    bool isFieldTechnical(rt::TrackField field)
    {
      auto const* def = rt::trackFieldDefinition(field);
      return def != nullptr && def->category == rt::TrackFieldCategory::Technical;
    }
  } // namespace

  BuiltInRow::BuiltInRow(rt::TrackField field)
    : field{field}
    , labelSlot{label, false}
    , valueClip{valueEditable, isFieldEditable(field), isFieldTechnical(field)}
    , valueSlot{valueBox, true}
    , editable{isFieldEditable(field)}
  {
    labelSlot.set_hexpand(false);
  }

  CompositeBuiltInRow::CompositeBuiltInRow(rt::TrackField primary, rt::TrackField secondary)
    : primaryField{primary}
    , secondaryField{secondary}
    , labelSlot{label, false}
    , primaryClip{primaryEditable, isFieldEditable(primary), isFieldTechnical(primary), false, true}
    , secondaryClip{secondaryEditable, isFieldEditable(secondary), isFieldTechnical(secondary), false, true}
    , valueSlot{valueBox, true}
    , primaryEditableFlag{isFieldEditable(primary)}
    , secondaryEditableFlag{isFieldEditable(secondary)}
  {
    valueBox.append(primaryClip);
    valueBox.append(separatorLabel);
    valueBox.append(secondaryClip);
  }

  CustomRow::CustomRow(std::string key, std::int32_t const /*actionSpacing*/)
    : key{std::move(key)}
    , labelSlot{label, false}
    , editable{}
    , deleteButton{}
    , valueClip{editable, true, false, true, false, &deleteButton}
    , valueSlot{valueBox, true}
  {
    labelSlot.set_hexpand(false);
  }

  SeparatorRow::SeparatorRow()
  {
    separator.set_margin_top(4);
    separator.set_margin_bottom(4);
    separator.set_hexpand(false);
    separator.set_halign(Gtk::Align::FILL);
  }
} // namespace ao::gtk::layout::track_field_grid
