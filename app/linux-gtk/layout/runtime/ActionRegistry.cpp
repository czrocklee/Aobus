// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "layout/runtime/ActionRegistry.h"

#include <ao/utility/Log.h>

#include <algorithm>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::gtk::layout
{
  ActionRegistry::ActionRegistry() = default;
  ActionRegistry::~ActionRegistry() = default;

  bool ActionRegistry::registerAction(ActionDescriptor descriptor,
                                      ActionHandler handler,
                                      ActionStateProvider stateProvider)
  {
    auto const it = std::ranges::find_if(_entries, [&](auto const& entry) { return entry.descriptor.id == descriptor.id; });

    if (it != _entries.end())
    {
      APP_LOG_ERROR("ActionRegistry: Duplicate registration for action id '{}'", descriptor.id);
      return false;
    }

    _entries.push_back({std::move(descriptor), std::move(handler), std::move(stateProvider)});
    return true;
  }

  std::optional<ActionDescriptor> ActionRegistry::descriptor(std::string_view id) const
  {
    auto const it = std::ranges::find_if(_entries, [&](auto const& entry) { return entry.descriptor.id == id; });

    if (it != _entries.end())
    {
      return it->descriptor;
    }

    return std::nullopt;
  }

  std::vector<ActionDescriptor> ActionRegistry::descriptors() const
  {
    auto result = std::vector<ActionDescriptor>{};
    result.reserve(_entries.size());

    for (auto const& entry : _entries)
    {
      result.push_back(entry.descriptor);
    }

    return result;
  }

  bool ActionRegistry::canBind(std::string_view id, ActionBindingContext const& ctx) const
  {
    auto const optDescriptor = descriptor(id);

    if (!optDescriptor)
    {
      return false;
    }

    auto const& desc = *optDescriptor;

    if (!ctx.hasAnchor && desc.capabilities.has(ActionCapability::RequiresAnchor))
    {
      return false;
    }

    if (!ctx.hasFocusedView && desc.capabilities.has(ActionCapability::RequiresFocusedView))
    {
      return false;
    }

    return true;
  }

  bool ActionRegistry::tryBind(std::string_view id, ActionBindingContext const& ctx) const
  {
    if (id == "none" || id.empty())
    {
      return false;
    }

    if (!canBind(id, ctx))
    {
      APP_LOG_DEBUG("ActionRegistry: Action '{}' cannot be bound to context (slot={}, anchor={})", 
                   id, static_cast<int>(ctx.slot), ctx.hasAnchor);
      return false;
    }

    return true;
  }

  ActionState ActionRegistry::state(std::string_view id, ActionActivationContext const& ctx) const
  {
    auto const it = std::ranges::find_if(_entries, [&](auto const& entry) { return entry.descriptor.id == id; });

    if (it != _entries.end() && it->stateProvider)
    {
      return it->stateProvider(ctx);
    }

    return ActionState{.enabled = true, .disabledReason = ""};
  }

  ActionActivationOutcome ActionRegistry::activate(std::string_view id, ActionActivationContext& ctx) const
  {
    auto const it = std::ranges::find_if(_entries, [&](auto const& entry) { return entry.descriptor.id == id; });

    if (it == _entries.end())
    {
      return ActionActivationOutcome{.result = ActionActivationResult::UnknownAction};
    }

    auto actionState = ActionState{.enabled = true, .disabledReason = ""};

    if (it->stateProvider)
    {
      actionState = it->stateProvider(ctx);

      if (!actionState.enabled)
      {
        return ActionActivationOutcome{.result = ActionActivationResult::Disabled, .state = std::move(actionState)};
      }
    }

    if (it->handler)
    {
      it->handler(ctx);
    }

    return ActionActivationOutcome{.result = ActionActivationResult::Activated, .state = std::move(actionState)};
  }

  ActionActivationOutcome ActionRegistry::tryActivate(std::string_view id, ActionActivationContext& ctx) const
  {
    auto outcome = activate(id, ctx);

    switch (outcome.result)
    {
      case ActionActivationResult::UnknownAction:
        APP_LOG_WARN("ActionRegistry: Attempt to activate unknown action id '{}'", id);
        break;
      case ActionActivationResult::Disabled:
      {
        APP_LOG_DEBUG("ActionRegistry: Action '{}' is disabled: {}", id, outcome.state.disabledReason);
        break;
      }
      case ActionActivationResult::Activated:
        APP_LOG_DEBUG("ActionRegistry: Activated action '{}' for component '{}'", id, ctx.componentId);
        break;
      case ActionActivationResult::InvalidBinding:
        APP_LOG_ERROR("ActionRegistry: Action '{}' has invalid binding for component '{}'", id, ctx.componentId);
        break;
    }

    return outcome;
  }
} // namespace ao::gtk::layout
