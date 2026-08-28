// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/uimodel/layout/component/LayoutSchema.h>

#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Gtk
{
  class Window;
  class Widget;
}

namespace ao::gtk::layout
{
  struct ActionActivationContext final
  {
    Gtk::Window& parentWindow;
    Gtk::Widget& anchorWidget;
    std::string componentId;
  };

  /// Whether a layout action can be activated in the current context.
  struct ActionAvailability final
  {
    bool enabled = true;
    std::string disabledReason;
  };

  using ActionHandler = std::function<void(ActionActivationContext&)>;
  using ActionStateProvider = std::function<ActionAvailability(ActionActivationContext const&)>;

  class ActionRegistry final
  {
  public:
    explicit ActionRegistry(uimodel::LayoutSchema& schema);
    ~ActionRegistry() = default;

    ActionRegistry(ActionRegistry const&) = delete;
    ActionRegistry& operator=(ActionRegistry const&) = delete;
    ActionRegistry(ActionRegistry&&) = delete;
    ActionRegistry& operator=(ActionRegistry&&) = delete;

    bool registerAction(uimodel::ActionSchema schema, ActionHandler handler, ActionStateProvider stateProvider = {});

    std::optional<uimodel::ActionSchema> action(std::string_view id) const;
    std::span<uimodel::ActionSchema const> actions() const;

    ActionAvailability state(std::string_view id, ActionActivationContext const& ctx) const;
    bool activate(std::string_view id, ActionActivationContext& ctx) const;

  private:
    struct Entry final
    {
      std::string id;
      ActionHandler handler;
      ActionStateProvider stateProvider;
    };

    uimodel::LayoutSchema& _schema;
    std::vector<Entry> _entries;
  };
} // namespace ao::gtk::layout
