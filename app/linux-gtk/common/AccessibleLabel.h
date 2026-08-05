// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <string_view>

namespace Gtk
{
  class Accessible;
  class Widget;
} // namespace Gtk

namespace ao::gtk
{
  void setAccessibleLabel(Gtk::Accessible& accessible, std::string_view label);
  void setTooltipAndAccessibleLabel(Gtk::Widget& widget, std::string_view label);
} // namespace ao::gtk
