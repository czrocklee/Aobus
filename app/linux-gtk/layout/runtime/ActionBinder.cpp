// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "ActionBinder.h"

#include "ActionRegistry.h"
#include <ao/rt/AppRuntime.h>
#include <ao/uimodel/layout/action/LayoutActionTypes.h>
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
                                           uimodel::LayoutActionSlot slot,
                                           Gtk::Widget& anchorWidget) const
  {
    auto const actionId = node.getProp<std::string>(std::string{propName}, std::string{defaultActionId});

    // TODO: hasAnchor and hasFocusedView are hardcoded for Phase 1 widget bindings
    auto const bindCtx = uimodel::LayoutActionBindingContext{
      .slot = slot, .hasAnchor = true, .hasFocusedView = true, .componentType = node.type};

    if (!_registry.tryBind(actionId, bindCtx))
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
      registry->tryActivate(actionId, actionCtx);
    };
  }
} // namespace ao::gtk::layout
