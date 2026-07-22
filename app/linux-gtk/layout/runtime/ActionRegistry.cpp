// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "layout/runtime/ActionRegistry.h"

#include <ao/rt/Log.h>
#include <ao/uimodel/layout/action/LayoutActionAvailability.h>
#include <ao/uimodel/layout/action/LayoutActionDescriptor.h>

#include <algorithm>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::gtk::layout
{
  ActionRegistry::ActionRegistry() = default;
  ActionRegistry::~ActionRegistry() = default;

  bool ActionRegistry::registerAction(uimodel::LayoutActionDescriptor descriptor,
                                      ActionHandler handler,
                                      ActionStateProvider stateProvider)
  {
    if (!_catalog.registerActionDescriptor(descriptor))
    {
      APP_LOG_ERROR("ActionRegistry: Duplicate registration for action id '{}'", descriptor.id);
      return false;
    }

    _entries.push_back({.id = descriptor.id, .handler = std::move(handler), .stateProvider = std::move(stateProvider)});
    return true;
  }

  std::optional<uimodel::LayoutActionDescriptor> ActionRegistry::descriptor(std::string_view id) const
  {
    return _catalog.descriptor(id);
  }

  std::vector<uimodel::LayoutActionDescriptor> ActionRegistry::descriptors() const
  {
    return _catalog.descriptors();
  }

  uimodel::LayoutActionAvailability ActionRegistry::state(std::string_view id, ActionActivationContext const& ctx) const
  {
    auto const it = std::ranges::find_if(_entries, [&](auto const& entry) { return entry.id == id; });

    if (it != _entries.end() && it->stateProvider)
    {
      return it->stateProvider(ctx);
    }

    return uimodel::LayoutActionAvailability{.enabled = true, .disabledReason = ""};
  }

  bool ActionRegistry::activate(std::string_view id, ActionActivationContext& ctx) const
  {
    auto const it = std::ranges::find_if(_entries, [&](auto const& entry) { return entry.id == id; });

    if (it == _entries.end())
    {
      APP_LOG_WARN("ActionRegistry: Attempt to activate unknown action id '{}'", id);
      return false;
    }

    if (it->stateProvider)
    {
      if (auto const actionState = it->stateProvider(ctx); !actionState.enabled)
      {
        APP_LOG_DEBUG("ActionRegistry: Action '{}' is disabled: {}", id, actionState.disabledReason);
        return false;
      }
    }

    if (it->handler)
    {
      it->handler(ctx);
    }

    APP_LOG_DEBUG("ActionRegistry: Activated action '{}' for component '{}'", id, ctx.componentId);
    return true;
  }

  uimodel::LayoutActionCatalog const& ActionRegistry::catalog() const noexcept
  {
    return _catalog;
  }
} // namespace ao::gtk::layout
