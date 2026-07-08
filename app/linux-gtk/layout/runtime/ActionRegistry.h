// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/uimodel/layout/action/LayoutActionActivation.h>
#include <ao/uimodel/layout/action/LayoutActionAvailability.h>
#include <ao/uimodel/layout/action/LayoutActionCatalog.h>
#include <ao/uimodel/layout/action/LayoutActionDescriptor.h>

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

namespace ao::uimodel
{
  struct LayoutActionBindingContext;
}

namespace ao::gtk::layout
{
  struct ActionActivationContext final
  {
    rt::AppRuntime& runtime;
    Gtk::Window& parentWindow;
    Gtk::Widget& anchorWidget;
    std::string componentId;
  };

  using ActionHandler = std::function<void(ActionActivationContext&)>;
  using ActionStateProvider = std::function<uimodel::LayoutActionAvailability(ActionActivationContext const&)>;

  class ActionRegistry final
  {
  public:
    ActionRegistry();
    ~ActionRegistry();

    ActionRegistry(ActionRegistry const&) = delete;
    ActionRegistry& operator=(ActionRegistry const&) = delete;
    ActionRegistry(ActionRegistry&&) = delete;
    ActionRegistry& operator=(ActionRegistry&&) = delete;

    bool registerAction(uimodel::LayoutActionDescriptor descriptor,
                        ActionHandler handler,
                        ActionStateProvider stateProvider = {});

    std::optional<uimodel::LayoutActionDescriptor> descriptor(std::string_view id) const;
    std::vector<uimodel::LayoutActionDescriptor> descriptors() const;

    bool canBind(std::string_view id, uimodel::LayoutActionBindingContext const& ctx) const;
    bool tryBind(std::string_view id, uimodel::LayoutActionBindingContext const& ctx) const;

    uimodel::LayoutActionAvailability state(std::string_view id, ActionActivationContext const& ctx) const;
    uimodel::LayoutActionActivationOutcome activate(std::string_view id, ActionActivationContext& ctx) const;
    uimodel::LayoutActionActivationOutcome tryActivate(std::string_view id, ActionActivationContext& ctx) const;

    uimodel::LayoutActionCatalog const& catalog() const noexcept;

  private:
    struct Entry final
    {
      std::string id;
      ActionHandler handler;
      ActionStateProvider stateProvider;
    };

    uimodel::LayoutActionCatalog _catalog;
    std::vector<Entry> _entries;
  };
} // namespace ao::gtk::layout
