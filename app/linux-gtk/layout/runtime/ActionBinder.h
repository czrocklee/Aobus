// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "layout/document/LayoutNode.h"
#include "layout/runtime/ActionRegistry.h"

#include <functional>
#include <string_view>

namespace Gtk { class Widget; }

namespace ao::gtk::layout
{
  struct LayoutContext;

  class ActionBinder final
  {
  public:
    ActionBinder(ActionRegistry const& registry,
                 rt::AppRuntime& runtime,
                 Gtk::Window& parentWindow);

    /**
     * @brief Binds a layout action to a property.
     * @param node The layout node.
     * @param propName The property name containing the action ID.
     * @param defaultActionId The default action ID if the property is missing.
     * @param slot The action slot.
     * @param anchorWidget The anchor widget for the action context. 
     *                     NOTE: The caller must ensure 'anchorWidget' outlives the returned function.
     * @return A function that activates the action.
     */
    std::function<void()> bind(LayoutNode const& node,
                               std::string_view propName,
                               std::string_view defaultActionId,
                               ActionSlot slot,
                               Gtk::Widget& anchorWidget) const;

  private:
    ActionRegistry const& _registry;
    rt::AppRuntime& _runtime;
    Gtk::Window& _parentWindow;
  };
} // namespace ao::gtk::layout
