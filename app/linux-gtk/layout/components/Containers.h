// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "layout/runtime/ComponentRegistry.h"

namespace ao::gtk::layout
{
  /**
   * @brief Register the built-in container components (box, split, scroll, spacer).
   */
  void registerContainerComponents(ComponentRegistry& registry);

  /**
   * @brief Apply common layout properties (margin, align, etc.) to a widget.
   */
  void applyCommonProps(Gtk::Widget& widget, LayoutNode const& node);
}
