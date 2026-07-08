// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "ActionRegistry.h"
#include <ao/uimodel/layout/action/LayoutActionDescriptor.h>

#include <giomm/actionmap.h>
#include <glibmm/refptr.h>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ao::gtk::layout
{
  class ActionContextProvider
  {
  public:
    ActionContextProvider() = default;
    virtual ~ActionContextProvider() = default;
    ActionContextProvider(ActionContextProvider const&) = delete;
    ActionContextProvider& operator=(ActionContextProvider const&) = delete;
    ActionContextProvider(ActionContextProvider&&) = delete;
    ActionContextProvider& operator=(ActionContextProvider&&) = delete;

    /**
     * @brief Returns a valid context for action activation via Gio.
     *
     * @param componentId Optional string specifying the component invoking the action.
     * @return ActionActivationContext for the given action.
     */
    virtual ActionActivationContext actionContext(std::string_view componentId = "") = 0;

    /**
     * @brief Checks if this context provider can supply a safe anchor widget for a specific action.
     * @param desc The descriptor of the action being checked.
     * @return true if a safe anchor can be guaranteed for the action's semantics.
     */
    virtual bool canProvideSafeAnchor([[maybe_unused]] uimodel::LayoutActionDescriptor const& desc) const
    {
      return false;
    }
  };

  class GioActionBridgeSession final
  {
  public:
    GioActionBridgeSession(ActionRegistry const& registry,
                           Gio::ActionMap& actionMap,
                           ActionContextProvider& contextProvider,
                           std::vector<std::string> exportedActionIds);

    void refreshStates();

  private:
    ActionRegistry const& _registry;
    Gio::ActionMap& _actionMap;
    ActionContextProvider& _contextProvider;
    std::vector<std::string> _exportedActionIds;
  };

  class GioActionBridge final
  {
  public:
    /**
     * @brief Exports pure command layout actions into a Gio::ActionMap (e.g. Gtk::Application or Gtk::Window).
     *
     * Only actions that do not require an anchor and do not present a menu will be exported in Phase 3a.
     *
     * @param registry The authoritative layout action registry.
     * @param actionMap The target action map where Gio::SimpleAction objects will be added.
     * @param contextProvider Provider that can construct a ActionActivationContext when the action is triggered.
     * @return A session object that can be used to refresh the states of the exported actions.
     */
    static std::unique_ptr<GioActionBridgeSession> exportActions(ActionRegistry const& registry,
                                                                 Gio::ActionMap& actionMap,
                                                                 ActionContextProvider& contextProvider);
  };
} // namespace ao::gtk::layout
