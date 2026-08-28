// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "GioActionBridge.h"

#include "ActionRegistry.h"
#include <ao/rt/Log.h>
#include <ao/uimodel/layout/component/LayoutSchema.h>

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
                                                 ActionContextProvider& contextProvider,
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
      auto gioActionPtr = _actionMap.lookup_action(id);

      if (!gioActionPtr)
      {
        continue;
      }

      if (auto simpleActionPtr = std::dynamic_pointer_cast<Gio::SimpleAction>(gioActionPtr); simpleActionPtr != nullptr)
      {
        auto ctx = _contextProvider.actionContext(id);
        auto const state = _registry.state(id, ctx);
        simpleActionPtr->set_enabled(state.enabled);
      }
    }
  }

  std::unique_ptr<GioActionBridgeSession> GioActionBridge::exportActions(ActionRegistry const& registry,
                                                                         Gio::ActionMap& actionMap,
                                                                         ActionContextProvider& contextProvider)
  {
    auto exportedActionIds = std::vector<std::string>{};
    auto const actionSchemas = registry.actions();

    for (auto const& actionSchema : actionSchemas)
    {
      // Phase 3c: support anchored or menu-presenting actions only when a context provider can supply the needed
      // parent/anchor safely.
      if (!contextProvider.canProvideSafeAnchor(actionSchema) &&
          (actionSchema.supports(uimodel::ActionCapability::RequiresAnchor) ||
           actionSchema.supports(uimodel::ActionCapability::PresentsMenu)))
      {
        APP_LOG_DEBUG("GioActionBridge: Skipping action {} due to missing context capabilities", actionSchema.id);
        continue;
      }

      auto actionPtr = Gio::SimpleAction::create(actionSchema.id);

      // Initialize the state based on the current context
      auto ctx = contextProvider.actionContext(actionSchema.id);
      auto const initialState = registry.state(actionSchema.id, ctx);
      actionPtr->set_enabled(initialState.enabled);

      actionPtr->signal_activate().connect(
        [&registry, &contextProvider, id = actionSchema.id](Glib::VariantBase const& /*parameter*/)
        {
          auto ctx = contextProvider.actionContext(id);
          registry.activate(id, ctx);
        });

      actionMap.add_action(actionPtr);
      exportedActionIds.push_back(actionSchema.id);
      APP_LOG_DEBUG("GioActionBridge: Exported action {}", actionSchema.id);
    }

    return std::make_unique<GioActionBridgeSession>(registry, actionMap, contextProvider, std::move(exportedActionIds));
  }
} // namespace ao::gtk::layout
