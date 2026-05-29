// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/uimodel/layout/ActionCatalog.h>
#include <ao/uimodel/layout/ActionTypes.h>

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ao::rt
{
  class AppRuntime;
}

namespace Gtk
{
  class Window;
  class Widget;
}

namespace ao::gtk::layout
{
  // Using aliases for shared action types
  using ActionCapability = uimodel::layout::ActionCapability;
  using ActionCapabilities = uimodel::layout::ActionCapabilities;
  using ActionDescriptor = uimodel::layout::ActionDescriptor;
  using ActionState = uimodel::layout::ActionState;
  using ActionSlot = uimodel::layout::ActionSlot;
  using ActionBindingProperty = uimodel::layout::ActionBindingProperty;
  using ActionBindingContext = uimodel::layout::ActionBindingContext;
  using ActionActivationResult = uimodel::layout::ActionActivationResult;
  using ActionActivationOutcome = uimodel::layout::ActionActivationOutcome;

  struct ActionActivationContext final
  {
    rt::AppRuntime& runtime;
    Gtk::Window& parentWindow;
    Gtk::Widget& anchorWidget;
    std::string componentId;
  };

  using ActionHandler = std::function<void(ActionActivationContext&)>;
  using ActionStateProvider = std::function<ActionState(ActionActivationContext const&)>;

  class ActionRegistry final
  {
  public:
    ActionRegistry();
    ~ActionRegistry();

    ActionRegistry(ActionRegistry const&) = delete;
    ActionRegistry& operator=(ActionRegistry const&) = delete;
    ActionRegistry(ActionRegistry&&) = delete;
    ActionRegistry& operator=(ActionRegistry&&) = delete;

    bool registerAction(ActionDescriptor descriptor, ActionHandler handler, ActionStateProvider stateProvider = {});

    std::optional<ActionDescriptor> descriptor(std::string_view id) const;
    std::vector<ActionDescriptor> descriptors() const;

    bool canBind(std::string_view id, ActionBindingContext const& ctx) const;
    bool tryBind(std::string_view id, ActionBindingContext const& ctx) const;

    ActionState state(std::string_view id, ActionActivationContext const& ctx) const;
    ActionActivationOutcome activate(std::string_view id, ActionActivationContext& ctx) const;
    ActionActivationOutcome tryActivate(std::string_view id, ActionActivationContext& ctx) const;

    uimodel::layout::ActionCatalog const& catalog() const noexcept;

  private:
    struct Entry final
    {
      std::string id;
      ActionHandler handler;
      ActionStateProvider stateProvider;
    };

    uimodel::layout::ActionCatalog _catalog;
    std::vector<Entry> _entries;
  };
} // namespace ao::gtk::layout
