// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <gtkmm/widget.h>

#include <cstdint>

namespace ao::gtk
{
  void measureImageWidgetForSquareAllocation(Gtk::Widget const& imageWidget, std::int32_t side);
} // namespace ao::gtk
