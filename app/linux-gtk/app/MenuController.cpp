// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/MenuController.h"

#include "app/WindowActionRegistry.h"
#include "i18n/GtkTextCatalog.h"
#include <ao/i18n/MessageCatalog.h>

#include <giomm/menu.h>

namespace ao::gtk
{
  void MenuController::setup(i18n::MessageCatalog const& textCatalog)
  {
    auto const text = [&textCatalog](i18n::MessageId const id) { return gtkText(textCatalog, id); };
    _menuModelPtr = Gio::Menu::create();

    auto fileMenuPtr = Gio::Menu::create();
    fileMenuPtr->append(text(i18n::MessageId::GtkShellOpenLibrary),
                        WindowActionRegistry::detailedWindowAction(WindowActionRegistry::kOpenLibrary));
    fileMenuPtr->append(text(i18n::MessageId::GtkShellScanLibrary),
                        WindowActionRegistry::detailedWindowAction(WindowActionRegistry::kScanLibrary));

    auto dataMenuPtr = Gio::Menu::create();
    dataMenuPtr->append(text(i18n::MessageId::GtkShellImportLibraryData),
                        WindowActionRegistry::detailedWindowAction(WindowActionRegistry::kImportLibrary));
    dataMenuPtr->append(text(i18n::MessageId::GtkShellExportLibraryData),
                        WindowActionRegistry::detailedWindowAction(WindowActionRegistry::kExportLibrary));
    fileMenuPtr->append_section(dataMenuPtr);

    fileMenuPtr->append(text(i18n::MessageId::GtkShellQuit), "app.quit");
    _menuModelPtr->append_submenu(text(i18n::MessageId::GtkShellMenuFile), fileMenuPtr);

    auto editMenuPtr = Gio::Menu::create();
    editMenuPtr->append(text(i18n::MessageId::GtkShellPreferences), "app.preferences");
    _menuModelPtr->append_submenu(text(i18n::MessageId::GtkShellMenuEdit), editMenuPtr);

    auto viewMenuPtr = Gio::Menu::create();
    viewMenuPtr->append(text(i18n::MessageId::GtkShellEditLayout),
                        WindowActionRegistry::detailedWindowAction(WindowActionRegistry::kEditLayout));
    viewMenuPtr->append(
      text(i18n::MessageId::GtkShellSavePanelSizesAsLayoutDefaults),
      WindowActionRegistry::detailedWindowAction(WindowActionRegistry::kSavePanelSizesAsLayoutDefaults));
    viewMenuPtr->append(text(i18n::MessageId::GtkShellResetRuntimeLayoutState),
                        WindowActionRegistry::detailedWindowAction(WindowActionRegistry::kResetRuntimeLayoutState));
    _menuModelPtr->append_submenu(text(i18n::MessageId::GtkShellMenuView), viewMenuPtr);

    auto helpMenuPtr = Gio::Menu::create();
    helpMenuPtr->append(text(i18n::MessageId::GtkShellAbout), "app.about");
    _menuModelPtr->append_submenu(text(i18n::MessageId::GtkShellMenuHelp), helpMenuPtr);
  }
} // namespace ao::gtk
