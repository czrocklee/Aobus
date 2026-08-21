// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/MenuController.h"

#include "app/WindowActionRegistry.h"
#include "i18n/GtkTextCatalog.h"

#include <giomm/menu.h>

namespace ao::gtk
{
  void MenuController::setup(GtkTextCatalog const& textCatalog)
  {
    auto const text = [&textCatalog](GtkTextId const id) { return std::string{textCatalog.text(id)}; };
    _menuModelPtr = Gio::Menu::create();

    auto fileMenuPtr = Gio::Menu::create();
    fileMenuPtr->append(
      text(GtkTextId::OpenLibrary), WindowActionRegistry::detailedWindowAction(WindowActionRegistry::kOpenLibrary));
    fileMenuPtr->append(
      text(GtkTextId::ScanLibrary), WindowActionRegistry::detailedWindowAction(WindowActionRegistry::kScanLibrary));

    auto dataMenuPtr = Gio::Menu::create();
    dataMenuPtr->append(text(GtkTextId::ImportLibraryData),
                        WindowActionRegistry::detailedWindowAction(WindowActionRegistry::kImportLibrary));
    dataMenuPtr->append(text(GtkTextId::ExportLibraryData),
                        WindowActionRegistry::detailedWindowAction(WindowActionRegistry::kExportLibrary));
    fileMenuPtr->append_section(dataMenuPtr);

    fileMenuPtr->append(text(GtkTextId::Quit), "app.quit");
    _menuModelPtr->append_submenu(text(GtkTextId::MenuFile), fileMenuPtr);

    auto editMenuPtr = Gio::Menu::create();
    editMenuPtr->append(text(GtkTextId::Preferences), "app.preferences");
    _menuModelPtr->append_submenu(text(GtkTextId::MenuEdit), editMenuPtr);

    auto viewMenuPtr = Gio::Menu::create();
    viewMenuPtr->append(
      text(GtkTextId::EditLayout), WindowActionRegistry::detailedWindowAction(WindowActionRegistry::kEditLayout));
    viewMenuPtr->append(
      text(GtkTextId::SavePanelSizesAsLayoutDefaults),
      WindowActionRegistry::detailedWindowAction(WindowActionRegistry::kSavePanelSizesAsLayoutDefaults));
    viewMenuPtr->append(text(GtkTextId::ResetRuntimeLayoutState),
                        WindowActionRegistry::detailedWindowAction(WindowActionRegistry::kResetRuntimeLayoutState));
    _menuModelPtr->append_submenu(text(GtkTextId::MenuView), viewMenuPtr);

    auto helpMenuPtr = Gio::Menu::create();
    helpMenuPtr->append(text(GtkTextId::About), "app.about");
    _menuModelPtr->append_submenu(text(GtkTextId::MenuHelp), helpMenuPtr);
  }
} // namespace ao::gtk
