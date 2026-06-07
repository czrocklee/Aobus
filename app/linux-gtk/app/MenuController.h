// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <giomm/menu.h>
#include <giomm/menumodel.h>
#include <glibmm/refptr.h>

#include <functional>

namespace Gtk
{
  class ApplicationWindow;
}

namespace ao::gtk
{
  namespace portal
  {
    class ImportExportCoordinator;
  }

  class MenuController final
  {
  public:
    MenuController(portal::ImportExportCoordinator& importExport, std::function<void()> onEditLayout);

    void setup(Gtk::ApplicationWindow& window);

    Glib::RefPtr<Gio::MenuModel> menuModel() const { return _menuModelPtr; }

  private:
    portal::ImportExportCoordinator& _importExport;
    std::function<void()> _onEditLayout;
    Glib::RefPtr<Gio::Menu> _menuModelPtr;
  };
} // namespace ao::gtk
