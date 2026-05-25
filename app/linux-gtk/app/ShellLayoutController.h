// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "layout/document/LayoutDocument.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/LayoutContext.h"
#include "layout/runtime/LayoutHost.h"
#include <ao/rt/async/LifetimeScope.h>

#include <gtkmm/window.h>

#include <string>

namespace ao::gtk
{
  class AppConfig;

  class ShellLayoutController final
  {
  public:
    explicit ShellLayoutController(rt::AppRuntime& runtime, Gtk::Window& parentWindow);

    layout::ComponentRegistry& registry() { return _registry; }
    layout::LayoutContext& context() { return _context; }
    layout::LayoutHost& host() { return _host; }
    layout::LayoutDocument const& activeLayout() const { return _activeLayout; }

    void attachToWindow();
    void loadLayout(AppConfig& config);
    void saveLayout(AppConfig& config) const;
    void openEditor(AppConfig& config);

  private:
    static void setupCss();

    layout::ComponentRegistry _registry;
    layout::LayoutContext _context;
    layout::LayoutHost _host;
    layout::LayoutDocument _activeLayout;
    std::string _activePresetId;
    bool _isCustomized = false;
    rt::async::LifetimeScope _tasks;
  };
} // namespace ao::gtk
