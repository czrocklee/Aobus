// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "ActionBinder.h"

#include "ActionRegistry.h"
#include <ao/rt/AppRuntime.h>
#include <ao/uimodel/layout/document/LayoutNode.h>

#include <gtkmm/widget.h>

#include <functional>
#include <string>
#include <string_view>

namespace ao::gtk::layout
{
  ActionBinder::ActionBinder(ActionRegistry const& registry, rt::AppRuntime& runtime, Gtk::Window& parentWindow)
    : _registry{registry}, _runtime{runtime}, _parentWindow{parentWindow}
  {
  }

  std::function<void()> ActionBinder::bind(uimodel::LayoutNode const& node,
                                           std::string_view propName,
                                           std::string_view defaultActionId,
                                           Gtk::Widget& anchorWidget) const
  {
    auto const actionId = node.propertyOr<std::string>(std::string{propName}, std::string{defaultActionId});

    if (actionId.empty() || actionId == "none" || !_registry.descriptor(actionId))
    {
      return {};
    }

    // Capture pointers to the dependencies to ensure the lambda uses the actual objects,
    // as the ActionBinder instance itself is typically short-lived (local to component ctor).
    return [registry = &_registry,
            runtime = &_runtime,
            parentWindow = &_parentWindow,
            actionId,
            anchor = &anchorWidget,
            nodeId = node.id]
    {
      auto actionCtx = ActionActivationContext{
        .runtime = *runtime, .parentWindow = *parentWindow, .anchorWidget = *anchor, .componentId = nodeId};
      registry->activate(actionId, actionCtx);
    };
  }
} // namespace ao::gtk::layout
