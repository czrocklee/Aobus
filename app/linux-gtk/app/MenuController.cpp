// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/MenuController.h"
#include "library_io/ImportExportCoordinator.h"
#include <giomm/menu.h>
#include <giomm/simpleaction.h>
#include <glibmm/variant.h>
#include <gtkmm/applicationwindow.h>

#include <functional>
#include <utility>

namespace ao::gtk
{
  MenuController::MenuController(ImportExportCoordinator& importExport, std::function<void()> onEditLayout)
    : _importExport{importExport}, _onEditLayout{std::move(onEditLayout)}
  {
  }

  void MenuController::setup(Gtk::ApplicationWindow& window)
  {
    _menuModel = Gio::Menu::create();

    auto fileMenu = Gio::Menu::create();
    fileMenu->append("Open Library", "win.open-library");
    fileMenu->append("Import Files", "win.import-files");
    fileMenu->append("Quit", "app.quit");
    _menuModel->append_submenu("File", fileMenu);

    auto viewMenu = Gio::Menu::create();
    viewMenu->append("Edit Layout...", "win.edit-layout");
    _menuModel->append_submenu("View", viewMenu);

    auto helpMenu = Gio::Menu::create();
    helpMenu->append("About", "app.about");
    _menuModel->append_submenu("Help", helpMenu);

    auto openAction = Gio::SimpleAction::create("open-library");
    openAction->signal_activate().connect([this](Glib::VariantBase const&) { _importExport.openLibrary(); });
    window.add_action(openAction);

    auto importAction = Gio::SimpleAction::create("import-files");
    importAction->signal_activate().connect([this](Glib::VariantBase const&) { _importExport.importFiles(); });
    window.add_action(importAction);

    auto exportLibAction = Gio::SimpleAction::create("export-library");
    exportLibAction->signal_activate().connect([this](Glib::VariantBase const&) { _importExport.exportLibrary(); });
    window.add_action(exportLibAction);

    auto importLibAction = Gio::SimpleAction::create("import-library");
    importLibAction->signal_activate().connect([this](Glib::VariantBase const&) { _importExport.importLibrary(); });
    window.add_action(importLibAction);

    auto editLayoutAction = Gio::SimpleAction::create("edit-layout");
    editLayoutAction->signal_activate().connect([this](Glib::VariantBase const&) { _onEditLayout(); });
    window.add_action(editLayoutAction);
  }
} // namespace ao::gtk
