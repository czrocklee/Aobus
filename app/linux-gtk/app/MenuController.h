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

  // Owns the main menu model and binds its window actions to the injected callbacks. The menu
  // structure and the action binding are deliberately kept together: the model is static data with
  // no behavior of its own, so splitting it out would only enable a low-value shape assertion.
  // Dispatch behavior is covered by MenuControllerTest.
  class MenuController final
  {
  public:
    MenuController(portal::ImportExportCoordinator& importExport,
                   std::function<void()> onEditLayout,
                   std::function<void()> onResetRuntimeLayoutState,
                   std::function<void()> onSaveCurrentPanelSizesAsLayoutDefaults,
                   std::function<void()> onEditKeyboardShortcuts);

    void setup(Gtk::ApplicationWindow& window);

    Glib::RefPtr<Gio::MenuModel> menuModel() const { return _menuModelPtr; }

  private:
    portal::ImportExportCoordinator& _importExport;
    std::function<void()> _onEditLayout;
    std::function<void()> _onResetRuntimeLayoutState;
    std::function<void()> _onSaveCurrentPanelSizesAsLayoutDefaults;
    std::function<void()> _onEditKeyboardShortcuts;
    Glib::RefPtr<Gio::Menu> _menuModelPtr;
  };
} // namespace ao::gtk
