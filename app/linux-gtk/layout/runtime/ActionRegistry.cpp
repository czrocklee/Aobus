// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "layout/runtime/ActionRegistry.h"

#include <ao/rt/Log.h>
#include <ao/uimodel/layout/component/LayoutSchema.h>

#include <algorithm>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::gtk::layout
{
  ActionRegistry::ActionRegistry(uimodel::LayoutSchema& schema)
    : _schema{schema}
  {
  }

  bool ActionRegistry::registerAction(uimodel::ActionSchema schema,
                                      ActionHandler handler,
                                      ActionStateProvider stateProvider)
  {
    if (!_schema.addAction(schema))
    {
      APP_LOG_ERROR("ActionRegistry: Duplicate registration for action id '{}'", schema.id);
      return false;
    }

    _entries.push_back({.id = schema.id, .handler = std::move(handler), .stateProvider = std::move(stateProvider)});
    return true;
  }

  std::optional<uimodel::ActionSchema> ActionRegistry::action(std::string_view id) const
  {
    return _schema.action(id);
  }

  std::span<uimodel::ActionSchema const> ActionRegistry::actions() const
  {
    return _schema.actions();
  }

  ActionAvailability ActionRegistry::state(std::string_view id, ActionActivationContext const& ctx) const
  {
    auto const it = std::ranges::find_if(_entries, [&](auto const& entry) { return entry.id == id; });

    if (it != _entries.end() && it->stateProvider)
    {
      return it->stateProvider(ctx);
    }

    return ActionAvailability{.enabled = true, .disabledReason = ""};
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
} // namespace ao::gtk::layout
