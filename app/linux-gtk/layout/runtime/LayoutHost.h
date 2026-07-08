// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "layout/runtime/LayoutComponent.h"
#include "layout/runtime/LayoutRuntime.h"

#include <gtkmm/box.h>

#include <memory>

namespace ao::uimodel
{
  struct LayoutDocument;
}

namespace ao::gtk::layout
{
  class ComponentRegistry;
  struct LayoutContext;

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
    void setLayout(LayoutContext& ctx, uimodel::LayoutDocument const& doc);

  private:
    LayoutRuntime _runtime;
    std::unique_ptr<LayoutComponent> _activeComponentPtr;
  };
} // namespace ao::gtk::layout
