// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <gtkmm/widget.h>

namespace ao::gtk::layout
{
  /**
   * @brief Base interface for all layout components.
   */
  class ILayoutComponent
  {
  public:
    virtual ~ILayoutComponent() = default;

    /**
     * @brief Get the underlying GTK widget.
     */
    virtual Gtk::Widget& widget() = 0;
  };
}
