// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/MenuController.h"

#include "portal/ImportExportActions.h"

#include <giomm/menu.h>
#include <giomm/simpleaction.h>
#include <glibmm/variant.h>
#include <gtkmm/applicationwindow.h>

#include <functional>
#include <utility>

namespace ao::gtk
{
  MenuController::MenuController(portal::ImportExportActions& importExport,
                                 std::function<void()> onEditLayout,
                                 std::function<void()> onResetRuntimeLayoutState,
                                 std::function<void()> onSaveCurrentPanelSizesAsLayoutDefaults,
                                 std::function<void()> onEditKeyboardShortcuts)
    : _importExport{importExport}
    , _onEditLayout{std::move(onEditLayout)}
    , _onResetRuntimeLayoutState{std::move(onResetRuntimeLayoutState)}
    , _onSaveCurrentPanelSizesAsLayoutDefaults{std::move(onSaveCurrentPanelSizesAsLayoutDefaults)}
    , _onEditKeyboardShortcuts{std::move(onEditKeyboardShortcuts)}
  {
  }

  void MenuController::setup(Gtk::ApplicationWindow& window)
  {
    _menuModelPtr = Gio::Menu::create();

    auto fileMenuPtr = Gio::Menu::create();
    fileMenuPtr->append("Open Library...", "win.open-library");
    fileMenuPtr->append("Scan Library", "win.scan-library");

    auto dataMenuPtr = Gio::Menu::create();
    dataMenuPtr->append("Import Library Data...", "win.import-library");
    dataMenuPtr->append("Export Library Data...", "win.export-library");
    fileMenuPtr->append_section(dataMenuPtr);

    fileMenuPtr->append("Quit", "app.quit");
    _menuModelPtr->append_submenu("File", fileMenuPtr);

    auto editMenuPtr = Gio::Menu::create();
    editMenuPtr->append("Keyboard Shortcuts...", "win.keyboard-shortcuts");
    _menuModelPtr->append_submenu("Edit", editMenuPtr);

    auto viewMenuPtr = Gio::Menu::create();
    viewMenuPtr->append("Edit Layout...", "win.edit-layout");
    viewMenuPtr->append("Save Current Panel Sizes as Layout Defaults", "win.save-panel-sizes-as-layout-defaults");
    viewMenuPtr->append("Reset Runtime Layout State", "win.reset-runtime-layout-state");
    _menuModelPtr->append_submenu("View", viewMenuPtr);

    auto helpMenuPtr = Gio::Menu::create();
    helpMenuPtr->append("About", "app.about");
    _menuModelPtr->append_submenu("Help", helpMenuPtr);

    auto openActionPtr = Gio::SimpleAction::create("open-library");
    openActionPtr->signal_activate().connect([this](Glib::VariantBase const&) { _importExport.openLibrary(); });
    window.add_action(openActionPtr);

    auto scanActionPtr = Gio::SimpleAction::create("scan-library");
    scanActionPtr->signal_activate().connect([this](Glib::VariantBase const&) { _importExport.scanLibrary(); });
    window.add_action(scanActionPtr);

    auto exportLibActionPtr = Gio::SimpleAction::create("export-library");
    exportLibActionPtr->signal_activate().connect([this](Glib::VariantBase const&) { _importExport.exportLibrary(); });
    window.add_action(exportLibActionPtr);

    auto importLibActionPtr = Gio::SimpleAction::create("import-library");
    importLibActionPtr->signal_activate().connect([this](Glib::VariantBase const&) { _importExport.importLibrary(); });
    window.add_action(importLibActionPtr);

    auto editLayoutActionPtr = Gio::SimpleAction::create("edit-layout");
    editLayoutActionPtr->signal_activate().connect([this](Glib::VariantBase const&) { _onEditLayout(); });
    window.add_action(editLayoutActionPtr);

    auto savePanelSizesActionPtr = Gio::SimpleAction::create("save-panel-sizes-as-layout-defaults");
    savePanelSizesActionPtr->signal_activate().connect([this](Glib::VariantBase const&)
                                                       { _onSaveCurrentPanelSizesAsLayoutDefaults(); });
    window.add_action(savePanelSizesActionPtr);

    auto resetRuntimeLayoutStateActionPtr = Gio::SimpleAction::create("reset-runtime-layout-state");
    resetRuntimeLayoutStateActionPtr->signal_activate().connect([this](Glib::VariantBase const&)
                                                                { _onResetRuntimeLayoutState(); });
    window.add_action(resetRuntimeLayoutStateActionPtr);

    auto keyboardShortcutsActionPtr = Gio::SimpleAction::create("keyboard-shortcuts");
    keyboardShortcutsActionPtr->signal_activate().connect([this](Glib::VariantBase const&)
                                                          { _onEditKeyboardShortcuts(); });
    window.add_action(keyboardShortcutsActionPtr);
  }
} // namespace ao::gtk
