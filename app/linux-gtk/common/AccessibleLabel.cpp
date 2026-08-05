// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "common/AccessibleLabel.h"

#include <glibmm/value.h>
#include <gtkmm/accessible.h>
#include <gtkmm/widget.h>

#include <string>
#include <string_view>

namespace ao::gtk
{
  void setAccessibleLabel(Gtk::Accessible& accessible, std::string_view const label)
  {
    auto value = Glib::Value<std::string>{};
    value.init(Glib::Value<std::string>::value_type());
    value.set(std::string{label});
    accessible.update_property(Gtk::Accessible::Property::LABEL, value);
  }

  void setTooltipAndAccessibleLabel(Gtk::Widget& widget, std::string_view const label)
  {
    widget.set_tooltip_text(std::string{label});
    setAccessibleLabel(widget, label);
  }
} // namespace ao::gtk
