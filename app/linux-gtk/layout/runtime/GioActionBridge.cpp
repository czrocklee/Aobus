// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "GioActionBridge.h"

#include "ActionRegistry.h"
#include <ao/rt/Log.h>
#include <ao/uimodel/layout/component/LayoutSchema.h>

#include <giomm/simpleaction.h>
#include <glibmm/variant.h>

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace ao::gtk::layout
{
  GioActionBridgeSession::GioActionBridgeSession(ActionRegistry const& registry,
                                                 Gio::ActionMap& actionMap,
                                                 ActionContextProvider& contextProvider,
                                                 std::size_t const expectedActionCount)
    : _registry{registry}, _contextProvider{contextProvider}, _registration{actionMap, expectedActionCount}
  {
    _exportedActions.reserve(expectedActionCount);
  }

  GioActionBridgeSession::GioActionBridgeSession(GioActionBridgeSession&& other) noexcept
    : _registry{other._registry}
    , _contextProvider{other._contextProvider}
    , _registration{std::move(other._registration)}
    , _exportedActions{std::move(other._exportedActions)}
  {
  }

  void GioActionBridgeSession::addExportedAction(std::string id, Glib::RefPtr<Gio::SimpleAction> actionPtr)
  {
    _exportedActions.push_back(ExportedAction{.id = std::move(id), .actionPtr = actionPtr});
    auto const& exported = _exportedActions.back();
    _registration.add(actionPtr,
                      [&registry = _registry, &contextProvider = _contextProvider, id = exported.id](
                        Glib::VariantBase const& /*parameter*/)
                      {
                        auto ctx = contextProvider.actionContext(id);
                        registry.activate(id, ctx);
                      });
  }

  void GioActionBridgeSession::refreshStates()
  {
    for (auto const& exported : _exportedActions)
    {
      if (!_registration.isCurrent(exported.actionPtr))
      {
        continue;
      }

      auto ctx = _contextProvider.actionContext(exported.id);
      auto const state = _registry.state(exported.id, ctx);
      exported.actionPtr->set_enabled(state.enabled);
    }
  }

  GioActionBridgeSession GioActionBridge::exportActions(ActionRegistry const& registry,
                                                        Gio::ActionMap& actionMap,
                                                        ActionContextProvider& contextProvider)
  {
    auto const actionSchemas = registry.actions();
    auto session = GioActionBridgeSession{registry, actionMap, contextProvider, actionSchemas.size()};

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

      session.addExportedAction(actionSchema.id, actionPtr);
      APP_LOG_DEBUG("GioActionBridge: Exported action {}", actionSchema.id);
    }

    return session;
  }
} // namespace ao::gtk::layout
