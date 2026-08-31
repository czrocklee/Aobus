// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "ActionRegistry.h"
#include "common/ActionMapRegistration.h"
#include <ao/uimodel/layout/component/LayoutSchema.h>

#include <giomm/actionmap.h>
#include <giomm/simpleaction.h>
#include <glibmm/refptr.h>

#include <cstddef>
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
     * @param actionSchema The action schema entry of the action being checked.
     * @return true if a safe anchor can be guaranteed for the action's semantics.
     */
    virtual bool canProvideSafeAnchor([[maybe_unused]] uimodel::ActionSchema const& actionSchema) const
    {
      return false;
    }
  };

  class [[nodiscard]] GioActionBridgeSession final
  {
  public:
    GioActionBridgeSession(ActionRegistry const& registry,
                           Gio::ActionMap& actionMap,
                           ActionContextProvider& contextProvider,
                           std::size_t expectedActionCount);
    ~GioActionBridgeSession() = default;

    GioActionBridgeSession(GioActionBridgeSession const&) = delete;
    GioActionBridgeSession& operator=(GioActionBridgeSession const&) = delete;
    GioActionBridgeSession(GioActionBridgeSession&& other) noexcept;
    GioActionBridgeSession& operator=(GioActionBridgeSession&&) = delete;

    void refreshStates();

  private:
    friend class GioActionBridge;

    struct ExportedAction final
    {
      std::string id;
      Glib::RefPtr<Gio::SimpleAction> actionPtr;
    };

    void addExportedAction(std::string id, Glib::RefPtr<Gio::SimpleAction> actionPtr);

    ActionRegistry const& _registry;
    ActionContextProvider& _contextProvider;
    ActionMapRegistration _registration;
    std::vector<ExportedAction> _exportedActions;
  };

  class GioActionBridge final
  {
  public:
    /**
     * @brief Exports layout actions into a Gio::ActionMap (e.g. Gtk::Application or Gtk::Window).
     *
     * Actions that require an anchor or present a menu are exported only when
     * the context provider can supply the required safe context.
     *
     * @param registry The authoritative layout action registry.
     * @param actionMap The target action map where Gio::SimpleAction objects will be added.
     * @param contextProvider Provider that can construct a ActionActivationContext when the action is triggered.
     * @return A session that owns the exported actions and activation connections.
     * The session must retire before the action map, registry, and context provider.
     */
    static GioActionBridgeSession exportActions(ActionRegistry const& registry,
                                                Gio::ActionMap& actionMap,
                                                ActionContextProvider& contextProvider);
  };
} // namespace ao::gtk::layout
