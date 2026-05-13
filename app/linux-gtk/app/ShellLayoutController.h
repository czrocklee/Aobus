// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "layout/document/LayoutDocument.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/LayoutDependencies.h"
#include "layout/runtime/LayoutHost.h"

namespace ao::rt
{
  class ConfigStore;
}

namespace ao::gtk
{
  class ShellLayoutController final
  {
  public:
    explicit ShellLayoutController(rt::AppSession& session, Gtk::Window& parentWindow);

    layout::ComponentRegistry& registry() { return _registry; }
    layout::LayoutDependencies& context() { return _context; }
    layout::LayoutHost& host() { return _host; }
    layout::LayoutDocument const& activeLayout() const { return _activeLayout; }

    void attachToWindow();
    void loadLayout(rt::ConfigStore& configStore);
    void saveLayout(rt::ConfigStore& configStore) const;
    void openEditor(rt::ConfigStore& configStore);

  private:
    static void setupCss();

    layout::ComponentRegistry _registry;
    layout::LayoutDependencies _context;
    layout::LayoutHost _host;
    layout::LayoutDocument _activeLayout;
  };
} // namespace ao::gtk
