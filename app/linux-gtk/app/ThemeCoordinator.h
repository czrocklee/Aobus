// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "app/AppConfig.h"
#include <ao/rt/StateTypes.h>

#include <gtkmm/widget.h>
#include <gtkmm/window.h>

#include <vector>

namespace ao::gtk
{
  class ThemeCoordinator final
  {
  public:
    void load(AppConfig const& config);
    void save(AppConfig& config) const;

    void setTheme(rt::ThemePresetId preset);
    rt::ThemePresetId activeTheme() const noexcept;

    void applyTo(Gtk::Widget& root) const;
    void registerToplevel(Gtk::Window& window);
    void unregisterToplevel(Gtk::Window& window);

  private:
    rt::ThemePresetId _activePreset = rt::ThemePresetId::Classic;
    std::vector<Gtk::Window*> _registeredWindows;

    void applyThemeClass(Gtk::Widget& widget, rt::ThemePresetId preset) const;
    void removeThemeClass(Gtk::Widget& widget, rt::ThemePresetId preset) const;
  };
} // namespace ao::gtk
