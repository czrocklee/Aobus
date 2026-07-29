// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "image/ImageWidgetLayout.h"

#include "common/WidgetMeasure.h"

#include <gtkmm/enums.h>
#include <gtkmm/widget.h>

#include <cstdint>

namespace ao::gtk
{
  void measureImageWidgetForSquareAllocation(Gtk::Widget const& imageWidget, std::int32_t side)
  {
    measureWidget(imageWidget, Gtk::Orientation::HORIZONTAL, -1);
    measureWidget(imageWidget, Gtk::Orientation::VERTICAL, side);
  }
} // namespace ao::gtk
