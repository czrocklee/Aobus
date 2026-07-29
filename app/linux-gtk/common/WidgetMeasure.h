// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <gtkmm/enums.h>
#include <gtkmm/widget.h>

#include <cstdint>

namespace ao::gtk
{
  struct WidgetMeasure final
  {
    std::int32_t minimum = 0;
    std::int32_t natural = 0;
  };

  WidgetMeasure measureWidget(Gtk::Widget const& widget, Gtk::Orientation orientation, std::int32_t forSize);
} // namespace ao::gtk
