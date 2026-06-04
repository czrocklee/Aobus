// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "TrackFieldGridRows.h"

#include "gtkmm/enums.h"
#include "track/TrackFieldUi.h"
#include <ao/rt/TrackField.h>

#include <cstdint>
#include <string>
#include <string_view>
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

  SectionHeaderRow::SectionHeaderRow([[maybe_unused]] std::string_view title)
  {
    button.set_has_frame(false);
    button.set_halign(Gtk::Align::FILL);
    button.set_hexpand(false);
    button.set_focusable(true);
    button.add_css_class("ao-track-detail-section-header");

    line.set_hexpand(true);
    line.set_valign(Gtk::Align::CENTER);
    line.add_css_class("ao-track-detail-section-line");

    icon.set_halign(Gtk::Align::END);
    icon.set_margin_start(4);

    box.set_hexpand(true);
    box.set_halign(Gtk::Align::FILL);
    box.set_margin_top(0);
    box.set_margin_bottom(0);
    box.append(line);
    box.append(icon);

    button.set_child(box);
    setExpanded(true);
  }

  void SectionHeaderRow::setExpanded(bool const expanded)
  {
    icon.set_from_icon_name(expanded ? "pan-down-symbolic" : "pan-end-symbolic");
  }

  void SectionHeaderRow::addCssClass(std::string_view const className)
  {
    button.add_css_class(std::string{className});
  }
} // namespace ao::gtk::layout::track_field_grid
