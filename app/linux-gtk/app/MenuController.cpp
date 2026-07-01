// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/MenuController.h"

#include "app/WindowActionRegistry.h"

#include <giomm/menu.h>

namespace ao::gtk
{
  void MenuController::setup()
  {
    _menuModelPtr = Gio::Menu::create();

    auto fileMenuPtr = Gio::Menu::create();
    fileMenuPtr->append(
      "Open Library...", WindowActionRegistry::detailedWindowAction(WindowActionRegistry::kOpenLibrary));
    fileMenuPtr->append("Scan Library", WindowActionRegistry::detailedWindowAction(WindowActionRegistry::kScanLibrary));

    auto dataMenuPtr = Gio::Menu::create();
    dataMenuPtr->append(
      "Import Library Data...", WindowActionRegistry::detailedWindowAction(WindowActionRegistry::kImportLibrary));
    dataMenuPtr->append(
      "Export Library Data...", WindowActionRegistry::detailedWindowAction(WindowActionRegistry::kExportLibrary));
    fileMenuPtr->append_section(dataMenuPtr);

    fileMenuPtr->append("Quit", "app.quit");
    _menuModelPtr->append_submenu("File", fileMenuPtr);

    auto editMenuPtr = Gio::Menu::create();
    editMenuPtr->append("Preferences...", "app.preferences");
    _menuModelPtr->append_submenu("Edit", editMenuPtr);

    auto viewMenuPtr = Gio::Menu::create();
    viewMenuPtr->append(
      "Edit Layout...", WindowActionRegistry::detailedWindowAction(WindowActionRegistry::kEditLayout));
    viewMenuPtr->append(
      "Save Current Panel Sizes as Layout Defaults",
      WindowActionRegistry::detailedWindowAction(WindowActionRegistry::kSavePanelSizesAsLayoutDefaults));
    viewMenuPtr->append("Reset Runtime Layout State",
                        WindowActionRegistry::detailedWindowAction(WindowActionRegistry::kResetRuntimeLayoutState));
    _menuModelPtr->append_submenu("View", viewMenuPtr);

    auto helpMenuPtr = Gio::Menu::create();
    helpMenuPtr->append("About", "app.about");
    _menuModelPtr->append_submenu("Help", helpMenuPtr);
  }
} // namespace ao::gtk
