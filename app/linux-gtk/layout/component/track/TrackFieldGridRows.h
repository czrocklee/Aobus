// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "gtkmm/enums.h"
#include "layout/component/track/TrackFieldGridWidgets.h"
#include <ao/Type.h>
#include <ao/rt/TrackField.h>

#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/image.h>
#include <gtkmm/label.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ao::gtk::layout::track_field_grid
{
  constexpr std::int32_t kCompositeFieldSpacing = 6;

  struct BuiltInRow final
  {
    rt::TrackField field;
    Gtk::Label label{};
    FixedHeightWidgetSlot labelSlot;
    Gtk::Box valueBox{Gtk::Orientation::HORIZONTAL, 0};
    FieldInlineEditor valueEditable{};
    FieldValueWrapper valueClip;
    FixedHeightWidgetSlot valueSlot;
    bool editable = false;
    bool discardNextEdit = false;

    explicit BuiltInRow(rt::TrackField field);
  };

  struct CompositeBuiltInRow final
  {
    rt::TrackField primaryField;
    rt::TrackField secondaryField;

    Gtk::Label label{};
    FixedHeightWidgetSlot labelSlot;

    Gtk::Box valueBox{Gtk::Orientation::HORIZONTAL, kCompositeFieldSpacing};

    FieldInlineEditor primaryEditable{};
    FieldValueWrapper primaryClip;

    Gtk::Label separatorLabel{"/"};

    FieldInlineEditor secondaryEditable{};
    FieldValueWrapper secondaryClip;

    FixedHeightWidgetSlot valueSlot;

    bool primaryEditableFlag = false;
    bool secondaryEditableFlag = false;
    bool discardNextEditPrimary = false;
    bool discardNextEditSecondary = false;

    CompositeBuiltInRow(rt::TrackField primary, rt::TrackField secondary);
  };

  struct CustomRow final
  {
    std::string key;
    Gtk::Label label{};
    FixedHeightWidgetSlot labelSlot;
    Gtk::Box valueBox{Gtk::Orientation::HORIZONTAL, 0};
    FieldInlineEditor editable{};
    Gtk::Button deleteButton{};
    FieldValueWrapper valueClip;
    Gtk::Image partialIcon{};
    FixedHeightWidgetSlot valueSlot;
    bool discardNextEdit = false;

    CustomRow(std::string key, std::int32_t actionSpacing);
  };

  struct SectionHeaderRow final
  {
    Gtk::Button button{};
    Gtk::Box box{Gtk::Orientation::HORIZONTAL, 0};
    Gtk::Box line{Gtk::Orientation::HORIZONTAL, 0};
    Gtk::Image icon{};

    explicit SectionHeaderRow(std::string_view title);
    void setExpanded(bool expanded);
    void addCssClass(std::string_view className);
  };

  struct UndoState final
  {
    std::string key;
    std::vector<TrackId> trackIds;
    std::string value;
  };
} // namespace ao::gtk::layout::track_field_grid
