// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "ActionBinder.h"

#include "ActionRegistry.h"
#include <ao/uimodel/layout/component/LayoutSchema.h>
#include <ao/uimodel/layout/document/LayoutNode.h>

#include <gtkmm/widget.h>

#include <functional>
#include <string>
#include <string_view>

namespace ao::gtk::layout
{
  ActionBinder::ActionBinder(ActionRegistry const& registry, Gtk::Window& parentWindow)
    : _registry{registry}, _parentWindow{parentWindow}
  {
  }

  std::function<void()> ActionBinder::bind(uimodel::LayoutNode const& node,
                                           uimodel::ComponentSchema const& schema,
                                           uimodel::ActionSlot const slot,
                                           Gtk::Widget& anchorWidget) const
  {
    auto const optActionId = schema.actionId(node, slot);

    if (!optActionId || !_registry.action(*optActionId))
    {
      return {};
    }

    auto const actionId = std::string{*optActionId};
    // Capture pointers to the dependencies to ensure the lambda uses the actual objects,
    // as the ActionBinder instance itself is typically short-lived (local to component ctor).
    return [registry = &_registry, parentWindow = &_parentWindow, actionId, anchor = &anchorWidget, nodeId = node.id]
    {
      auto actionCtx =
        ActionActivationContext{.parentWindow = *parentWindow, .anchorWidget = *anchor, .componentId = nodeId};
      registry->activate(actionId, actionCtx);
    };
  }
} // namespace ao::gtk::layout
