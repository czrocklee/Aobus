// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "layout/runtime/ActionRegistry.h"

#include <ao/rt/Log.h>
#include <ao/uimodel/layout/action/LayoutActionActivation.h>
#include <ao/uimodel/layout/action/LayoutActionAvailability.h>
#include <ao/uimodel/layout/action/LayoutActionBinding.h>
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

  bool ActionRegistry::canBind(std::string_view id, uimodel::LayoutActionBindingContext const& ctx) const
  {
    return _catalog.canBind(id, ctx);
  }

  bool ActionRegistry::tryBind(std::string_view id, uimodel::LayoutActionBindingContext const& ctx) const
  {
    if (id == "none" || id.empty())
    {
      return false;
    }

    if (!_catalog.canBind(id, ctx))
    {
      APP_LOG_DEBUG("ActionRegistry: Action '{}' cannot be bound to context (slot={}, anchor={})",
                    id,
                    static_cast<int>(ctx.slot),
                    ctx.hasAnchor);
      return false;
    }

    return true;
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

  uimodel::LayoutActionActivationResult ActionRegistry::activate(std::string_view id,
                                                                 ActionActivationContext& ctx) const
  {
    auto const it = std::ranges::find_if(_entries, [&](auto const& entry) { return entry.id == id; });

    if (it == _entries.end())
    {
      return uimodel::LayoutActionActivationResult{.outcome = uimodel::LayoutActionActivationOutcome::UnknownAction};
    }

    auto actionState = uimodel::LayoutActionAvailability{.enabled = true, .disabledReason = ""};

    if (it->stateProvider)
    {
      actionState = it->stateProvider(ctx);

      if (!actionState.enabled)
      {
        return uimodel::LayoutActionActivationResult{
          .outcome = uimodel::LayoutActionActivationOutcome::Disabled, .state = std::move(actionState)};
      }
    }

    if (it->handler)
    {
      it->handler(ctx);
    }

    return uimodel::LayoutActionActivationResult{
      .outcome = uimodel::LayoutActionActivationOutcome::Activated, .state = std::move(actionState)};
  }

  uimodel::LayoutActionActivationResult ActionRegistry::tryActivate(std::string_view id,
                                                                    ActionActivationContext& ctx) const
  {
    auto result = activate(id, ctx);

    switch (result.outcome)
    {
      case uimodel::LayoutActionActivationOutcome::UnknownAction:
        APP_LOG_WARN("ActionRegistry: Attempt to activate unknown action id '{}'", id);
        break;
      case uimodel::LayoutActionActivationOutcome::Disabled:
        APP_LOG_DEBUG("ActionRegistry: Action '{}' is disabled: {}", id, result.state.disabledReason);
        break;
      case uimodel::LayoutActionActivationOutcome::Activated:
        APP_LOG_DEBUG("ActionRegistry: Activated action '{}' for component '{}'", id, ctx.componentId);
        break;
      case uimodel::LayoutActionActivationOutcome::InvalidBinding:
        APP_LOG_ERROR("ActionRegistry: Action '{}' has invalid binding for component '{}'", id, ctx.componentId);
        break;
    }

    return result;
  }

  uimodel::LayoutActionCatalog const& ActionRegistry::catalog() const noexcept
  {
    return _catalog;
  }
} // namespace ao::gtk::layout
