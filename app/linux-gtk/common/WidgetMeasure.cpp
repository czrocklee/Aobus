// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "common/WidgetMeasure.h"

#include <gtkmm/enums.h>
#include <gtkmm/widget.h>

#include <cstdint>

namespace ao::gtk
{
  WidgetMeasure measureWidget(Gtk::Widget const& widget, Gtk::Orientation orientation, std::int32_t forSize)
  {
    auto result = WidgetMeasure{};
    std::int32_t minimumBaseline = -1;
    std::int32_t naturalBaseline = -1;
    widget.measure(orientation, forSize, result.minimum, result.natural, minimumBaseline, naturalBaseline);
    return result;
  }
} // namespace ao::gtk
