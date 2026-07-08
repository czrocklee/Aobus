// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <gtkmm/widget.h>

namespace ao::gtk::layout
{
  /**
   * @brief Base interface for all layout components.
   */
  class LayoutComponent
  {
  public:
    LayoutComponent() = default;
    virtual ~LayoutComponent() = default;

    // Not copyable or movable
    LayoutComponent(LayoutComponent const&) = delete;
    LayoutComponent& operator=(LayoutComponent const&) = delete;
    LayoutComponent(LayoutComponent&&) = delete;
    LayoutComponent& operator=(LayoutComponent&&) = delete;

    /**
     * @brief Get the underlying GTK widget.
     */
    virtual Gtk::Widget& widget() = 0;
  };
} // namespace ao::gtk::layout
