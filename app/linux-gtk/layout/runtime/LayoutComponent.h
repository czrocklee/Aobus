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

    /**
     * @brief Reconcile component-managed widget state with the authored layout properties.
     *
     * The registry applies authored common properties after construction, so a component that also
     * drives one of those properties at runtime would otherwise be overwritten by the author's value.
     * Components in that position combine both sources here; everyone else needs no reconciliation.
     */
    virtual void onAuthoredPropsApplied() {}
  };
} // namespace ao::gtk::layout
