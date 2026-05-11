// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "ComponentContext.h"
#include "LayoutDocument.h"
#include "LayoutRuntime.h"

#include <gtkmm/box.h>
#include <memory>

namespace ao::gtk::layout
{
  /**
   * @brief A GTK widget that hosts a dynamic layout.
   */
  class LayoutHost final : public Gtk::Box
  {
  public:
    explicit LayoutHost(ComponentRegistry const& registry);

    /**
     * @brief Set the active layout.
     *
     * This will rebuild the entire layout tree.
     */
    void setLayout(ComponentContext& ctx, LayoutDocument const& doc);

  private:
    LayoutRuntime _runtime;
    std::unique_ptr<ILayoutComponent> _activeComponent;
  };
} // namespace ao::gtk::layout
