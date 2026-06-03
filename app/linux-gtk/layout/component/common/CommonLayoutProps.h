// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "layout/document/LayoutNode.h"

#include <gtkmm/widget.h>

namespace ao::gtk::layout
{
  /**
   * @brief Apply common layout properties (align, sizing, etc.) to a widget.
   */
  void applyCommonProps(Gtk::Widget& widget, LayoutNode const& node);
}
