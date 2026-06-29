// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "TrackFieldGridRows.h"

#include "gtkmm/enums.h"
#include "layout/component/track/TrackFieldGridWidgets.h"
#include "track/TrackFieldUi.h"
#include <ao/rt/TrackField.h>
#include <ao/uimodel/field/TrackFieldEditPolicy.h>

#include <pangomm/layout.h>

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
      return uiDef != nullptr && uiDef->parseInlineEdit != nullptr && uimodel::trackFieldCanWritePatch(field);
    }

    bool isFieldTechnical(rt::TrackField field)
    {
      auto const* def = rt::trackFieldDefinition(field);
      return def != nullptr && def->category == rt::TrackFieldCategory::Technical;
    }

    std::string fieldSlotCssClass(rt::TrackField const field, std::string_view const slotName)
    {
      auto className = std::string{"ao-track-field-grid-field-"};
      className += std::string{rt::trackFieldId(field)};
      className += "-";
      className += std::string{slotName};
      className += "-slot";
      return className;
    }

    void addFieldSlotClasses(FixedHeightWidgetSlot& labelSlot, FixedHeightWidgetSlot& valueSlot, rt::TrackField field)
    {
      labelSlot.add_css_class("ao-track-field-grid-label-slot");
      labelSlot.add_css_class(fieldSlotCssClass(field, "label"));
      valueSlot.add_css_class("ao-track-field-grid-value-slot");
      valueSlot.add_css_class(fieldSlotCssClass(field, "value"));
    }
  } // namespace

  BuiltInRow::BuiltInRow(rt::TrackField field)
    : field{field}, labelSlot{label, false}, valueSlot{valueBox, true}, editable{isFieldEditable(field)}
  {
    labelSlot.set_hexpand(false);
    addFieldSlotClasses(labelSlot, valueSlot, field);
    valueEditor.setEditable(editable);

    if (isFieldTechnical(field))
    {
      valueEditor.add_css_class("ao-detail-field-technical");
    }

    valueBox.append(valueEditor);
  }

  CompositeBuiltInRow::CompositeBuiltInRow(rt::TrackField primary, rt::TrackField secondary)
    : primaryField{primary}
    , secondaryField{secondary}
    , labelSlot{label, false}
    , valueSlot{valueBox, true}
    , primaryEditableFlag{isFieldEditable(primary)}
    , secondaryEditableFlag{isFieldEditable(secondary)}
  {
    addFieldSlotClasses(labelSlot, valueSlot, primary);
    valueSlot.add_css_class(fieldSlotCssClass(secondary, "value"));
    primaryEditor.setEditable(primaryEditableFlag);
    secondaryEditor.setEditable(secondaryEditableFlag);
    valueBox.append(primaryEditor);
    valueBox.append(separatorLabel);
    valueBox.append(secondaryEditor);
  }

  CustomRow::CustomRow(std::string key, std::int32_t const /*actionSpacing*/)
    : key{std::move(key)}, labelSlot{label, false}, deleteButton{}, valueSlot{valueBox, true}
  {
    labelSlot.set_hexpand(false);
    labelSlot.add_css_class("ao-track-field-grid-custom-label-slot");
    valueSlot.add_css_class("ao-track-field-grid-custom-value-slot");
    editor.setEditable(true);
    valueBox.add_css_class("ao-detail-custom-row");
    valueBox.append(editor);
    valueBox.append(partialIcon);
    valueBox.append(deleteButton);
  }

  SectionHeaderRow::SectionHeaderRow(std::string_view title)
  {
    button.set_has_frame(false);
    button.set_halign(Gtk::Align::FILL);
    button.set_hexpand(false);
    button.set_focusable(true);
    button.add_css_class("ao-track-detail-section-header");

    line.set_hexpand(true);
    line.set_valign(Gtk::Align::CENTER);
    line.add_css_class("ao-track-detail-section-line");

    // The disclosure chevron floats over the leading end of the full-bleed line so it never
    // displaces it. CSS keeps it invisible until the header is hovered or focused, leaving a
    // clean uninterrupted rule at rest while still exposing the collapse affordance on demand.
    icon.set_halign(Gtk::Align::START);
    icon.set_valign(Gtk::Align::CENTER);
    icon.set_margin_start(2);
    icon.add_css_class("ao-track-detail-section-chevron");

    label.set_text(std::string{title});
    label.set_halign(Gtk::Align::FILL);
    label.set_valign(Gtk::Align::CENTER);
    label.set_hexpand(true);
    label.set_xalign(0.0F);
    label.set_ellipsize(Pango::EllipsizeMode::END);
    label.set_size_request(0, -1);

    overlay.set_hexpand(true);
    overlay.set_halign(Gtk::Align::FILL);
    overlay.set_child(line);
    overlay.add_overlay(icon);
    overlay.add_overlay(label);
    overlay.set_measure_overlay(icon, true);
    overlay.set_measure_overlay(label, false);

    button.set_child(overlay);
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
