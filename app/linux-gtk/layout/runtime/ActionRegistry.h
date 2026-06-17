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
  struct ActionActivationContext final
  {
    rt::AppRuntime& runtime;
    Gtk::Window& parentWindow;
    Gtk::Widget& anchorWidget;
    std::string componentId;
  };

  using ActionHandler = std::function<void(ActionActivationContext&)>;
  using ActionStateProvider = std::function<uimodel::layout::ActionState(ActionActivationContext const&)>;

  class ActionRegistry final
  {
  public:
    ActionRegistry();
    ~ActionRegistry();

    ActionRegistry(ActionRegistry const&) = delete;
    ActionRegistry& operator=(ActionRegistry const&) = delete;
    ActionRegistry(ActionRegistry&&) = delete;
    ActionRegistry& operator=(ActionRegistry&&) = delete;

    bool registerAction(uimodel::layout::ActionDescriptor descriptor,
                        ActionHandler handler,
                        ActionStateProvider stateProvider = {});

    std::optional<uimodel::layout::ActionDescriptor> descriptor(std::string_view id) const;
    std::vector<uimodel::layout::ActionDescriptor> descriptors() const;

    bool canBind(std::string_view id, uimodel::layout::ActionBindingContext const& ctx) const;
    bool tryBind(std::string_view id, uimodel::layout::ActionBindingContext const& ctx) const;

    uimodel::layout::ActionState state(std::string_view id, ActionActivationContext const& ctx) const;
    uimodel::layout::ActionActivationOutcome activate(std::string_view id, ActionActivationContext& ctx) const;
    uimodel::layout::ActionActivationOutcome tryActivate(std::string_view id, ActionActivationContext& ctx) const;

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
