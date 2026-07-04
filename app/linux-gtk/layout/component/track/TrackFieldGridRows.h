// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "gtkmm/enums.h"
#include "layout/component/track/TrackFieldGridWidgets.h"
#include <ao/rt/TrackField.h>

#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/image.h>
#include <gtkmm/label.h>
#include <gtkmm/overlay.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace ao::gtk::layout::track_field_grid
{
  constexpr std::int32_t kCompositeFieldSpacing = 6;

  struct BuiltInRow final
  {
    rt::TrackField field;
    Gtk::Label label{};
    FixedHeightWidgetSlot labelSlot;
    Gtk::Box valueBox{Gtk::Orientation::HORIZONTAL, 0};
    DetailFieldEditor valueEditor{};
    FixedHeightWidgetSlot valueSlot;
    bool editable = false;

    explicit BuiltInRow(rt::TrackField field);
  };

  struct CompositeBuiltInRow final
  {
    rt::TrackField primaryField;
    rt::TrackField secondaryField;

    Gtk::Label label{};
    FixedHeightWidgetSlot labelSlot;

    Gtk::Box valueBox{Gtk::Orientation::HORIZONTAL, kCompositeFieldSpacing};

    DetailFieldEditor primaryEditor{};

    Gtk::Label separatorLabel{"/"};

    DetailFieldEditor secondaryEditor{};

    FixedHeightWidgetSlot valueSlot;

    bool primaryEditableFlag = false;
    bool secondaryEditableFlag = false;
    CompositeBuiltInRow(rt::TrackField primary, rt::TrackField secondary);
  };

  struct CustomRow final
  {
    std::string key;
    Gtk::Label label{};
    FixedHeightWidgetSlot labelSlot;
    Gtk::Box valueBox{Gtk::Orientation::HORIZONTAL, 0};
    DetailFieldEditor editor{};
    Gtk::Button deleteButton{};
    Gtk::Image partialIcon{};
    FixedHeightWidgetSlot valueSlot;

    CustomRow(std::string key, std::int32_t actionSpacing);
  };

  struct SectionHeaderRow final
  {
    Gtk::Button button{};
    Gtk::Overlay overlay{};
    Gtk::Label label{};
    Gtk::Box line{Gtk::Orientation::HORIZONTAL, 0};
    Gtk::Image icon{};

    explicit SectionHeaderRow(std::string_view title);
    void setExpanded(bool expanded);
    void addCssClass(std::string_view className);
  };
} // namespace ao::gtk::layout::track_field_grid
