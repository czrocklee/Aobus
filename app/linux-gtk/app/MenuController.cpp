// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/MenuController.h"

#include "app/WindowActionRegistry.h"
#include "i18n/GtkText.h"
#include <ao/i18n/MessageCatalog.h>

#include <giomm/menu.h>

namespace ao::gtk
{
  MenuController::MenuController(i18n::MessageCatalog const& textCatalog)
  {
    auto const text = [&textCatalog](i18n::MessageId const id) { return gtkText(textCatalog, id); };
    _menuModelPtr = Gio::Menu::create();

    auto fileMenuPtr = Gio::Menu::create();
    fileMenuPtr->append(text(i18n::MessageId::ShellOpenLibrary),
                        WindowActionRegistry::detailedWindowAction(WindowActionRegistry::kOpenLibrary));
    fileMenuPtr->append(text(i18n::MessageId::ShellScanLibrary),
                        WindowActionRegistry::detailedWindowAction(WindowActionRegistry::kScanLibrary));

    auto dataMenuPtr = Gio::Menu::create();
    dataMenuPtr->append(text(i18n::MessageId::ShellImportLibraryData),
                        WindowActionRegistry::detailedWindowAction(WindowActionRegistry::kImportLibrary));
    dataMenuPtr->append(text(i18n::MessageId::ShellExportLibraryData),
                        WindowActionRegistry::detailedWindowAction(WindowActionRegistry::kExportLibrary));
    fileMenuPtr->append_section(dataMenuPtr);

    fileMenuPtr->append(text(i18n::MessageId::GtkShellQuit), "app.quit");
    _menuModelPtr->append_submenu(text(i18n::MessageId::ShellMenuFile), fileMenuPtr);

    auto editMenuPtr = Gio::Menu::create();
    editMenuPtr->append(text(i18n::MessageId::GtkShellPreferences), "app.preferences");
    _menuModelPtr->append_submenu(text(i18n::MessageId::ShellMenuEdit), editMenuPtr);

    auto viewMenuPtr = Gio::Menu::create();
    viewMenuPtr->append(text(i18n::MessageId::GtkShellEditLayout),
                        WindowActionRegistry::detailedWindowAction(WindowActionRegistry::kEditLayout));
    viewMenuPtr->append(
      text(i18n::MessageId::GtkShellSavePanelSizesAsLayoutDefaults),
      WindowActionRegistry::detailedWindowAction(WindowActionRegistry::kSavePanelSizesAsLayoutDefaults));
    viewMenuPtr->append(text(i18n::MessageId::GtkShellResetRuntimeLayoutState),
                        WindowActionRegistry::detailedWindowAction(WindowActionRegistry::kResetRuntimeLayoutState));
    _menuModelPtr->append_submenu(text(i18n::MessageId::ShellMenuView), viewMenuPtr);

    auto helpMenuPtr = Gio::Menu::create();
    helpMenuPtr->append(text(i18n::MessageId::GtkShellAbout), "app.about");
    _menuModelPtr->append_submenu(text(i18n::MessageId::ShellMenuHelp), helpMenuPtr);
  }
} // namespace ao::gtk
