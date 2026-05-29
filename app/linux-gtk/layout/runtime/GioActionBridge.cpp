// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "GioActionBridge.h"

#include "ActionRegistry.h"
#include <ao/utility/Log.h>

#include <giomm/simpleaction.h>
#include <glibmm/variant.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace ao::gtk::layout
{
  GioActionBridgeSession::GioActionBridgeSession(ActionRegistry const& registry,
                                                 Gio::ActionMap& actionMap,
                                                 IActionContextProvider& contextProvider,
                                                 std::vector<std::string> exportedActionIds)
    : _registry{registry}
    , _actionMap{actionMap}
    , _contextProvider{contextProvider}
    , _exportedActionIds{std::move(exportedActionIds)}
  {
  }

  void GioActionBridgeSession::refreshStates()
  {
    for (auto const& id : _exportedActionIds)
    {
      auto gioAction = _actionMap.lookup_action(id);

      if (!gioAction)
      {
        continue;
      }

      if (auto simpleAction = std::dynamic_pointer_cast<Gio::SimpleAction>(gioAction))
      {
        auto ctx = _contextProvider.getActionContext(id);
        auto const state = _registry.state(id, ctx);
        simpleAction->set_enabled(state.enabled);
      }
    }
  }

  std::unique_ptr<GioActionBridgeSession> GioActionBridge::exportActions(ActionRegistry const& registry,
                                                                         Gio::ActionMap& actionMap,
                                                                         IActionContextProvider& contextProvider)
  {
    auto exportedActionIds = std::vector<std::string>{};
    auto const& descriptors = registry.descriptors();

    for (auto const& desc : descriptors)
    {
      // Phase 3c: support anchored or menu-presenting actions only when a context provider can supply the needed
      // parent/anchor safely.
      if (!contextProvider.canProvideSafeAnchor(desc) && (desc.capabilities.has(ActionCapability::RequiresAnchor) ||
                                                          desc.capabilities.has(ActionCapability::PresentsMenu)))
      {
        APP_LOG_DEBUG("GioActionBridge: Skipping action {} due to missing context capabilities", desc.id);
        continue;
      }

      auto action = Gio::SimpleAction::create(desc.id);

      // Initialize the state based on the current context
      auto ctx = contextProvider.getActionContext(desc.id);
      auto const initialState = registry.state(desc.id, ctx);
      action->set_enabled(initialState.enabled);

      action->signal_activate().connect(
        [&registry, &contextProvider, id = desc.id](Glib::VariantBase const& /*param*/)
        {
          auto ctx = contextProvider.getActionContext(id);
          registry.tryActivate(id, ctx);
        });

      actionMap.add_action(action);
      exportedActionIds.push_back(desc.id);
      APP_LOG_DEBUG("GioActionBridge: Exported action {}", desc.id);
    }

    return std::make_unique<GioActionBridgeSession>(registry, actionMap, contextProvider, std::move(exportedActionIds));
  }
} // namespace ao::gtk::layout
