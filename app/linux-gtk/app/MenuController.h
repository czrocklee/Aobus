// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <giomm/menu.h>
#include <giomm/menumodel.h>
#include <glibmm/refptr.h>
#include <gtkmm/applicationwindow.h>

#include <functional>

namespace ao::gtk
{
  class ImportExportCoordinator;

  class MenuController final
  {
  public:
    MenuController(ImportExportCoordinator& importExport, std::function<void()> onEditLayout);

    void setup(Gtk::ApplicationWindow& window);

    Glib::RefPtr<Gio::MenuModel> menuModel() const { return _menuModel; }

  private:
    ImportExportCoordinator& _importExport;
    std::function<void()> _onEditLayout;
    Glib::RefPtr<Gio::Menu> _menuModel;
  };
} // namespace ao::gtk
