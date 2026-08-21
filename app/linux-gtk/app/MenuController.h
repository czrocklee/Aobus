// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <giomm/menu.h>
#include <giomm/menumodel.h>
#include <glibmm/refptr.h>

namespace ao::gtk
{
  class GtkTextCatalog;
  class WindowActionRegistry;

  // Owns the main menu model. Window-scoped action installation lives in WindowActionRegistry so
  // the menu and Gio action declarations do not drift.
  class MenuController final
  {
  public:
    MenuController() = default;

    void setup(GtkTextCatalog const& textCatalog);

    Glib::RefPtr<Gio::MenuModel> menuModel() const { return _menuModelPtr; }

  private:
    Glib::RefPtr<Gio::Menu> _menuModelPtr;
  };
} // namespace ao::gtk
