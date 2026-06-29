// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors
#include "app/ThemeCoordinator.h"

#include "app/AppConfig.h"
#include "app/ThemePreset.h"
#include <ao/rt/AppPrefsState.h>

#include <gtkmm/widget.h>
#include <gtkmm/window.h>

#include <algorithm>
#include <string>

namespace ao::gtk
{
  ThemeRegistrationToken::~ThemeRegistrationToken()
  {
    reset();
  }

  void ThemeRegistrationToken::reset()
  {
    if (_coordinator != nullptr && _window != nullptr)
    {
      _coordinator->unregisterToplevel(*_window);
    }

    _coordinator = nullptr;
    _window = nullptr;
  }

  void ThemeCoordinator::load(AppConfig const& config)
  {
    auto prefs = rt::AppPrefsState{};
    config.loadAppPrefs(prefs);
    _activePreset = themePresetFromString(prefs.lastThemePreset);
  }

  void ThemeCoordinator::save(AppConfig& config) const
  {
    auto prefs = rt::AppPrefsState{};
    config.loadAppPrefs(prefs);
    prefs.lastThemePreset = std::string{themePresetToString(_activePreset)};
    config.saveAppPrefs(prefs);
  }

  void ThemeCoordinator::setTheme(rt::ThemePresetId preset)
  {
    if (_activePreset == preset)
    {
      return;
    }

    auto const oldPreset = _activePreset;
    _activePreset = preset;

    for (auto* const window : _registeredWindows)
    {
      if (window != nullptr)
      {
        removeThemeClass(*window, oldPreset);
        applyThemeClass(*window, preset);
      }
    }
  }

  rt::ThemePresetId ThemeCoordinator::activeTheme() const noexcept
  {
    return _activePreset;
  }

  void ThemeCoordinator::applyTo(Gtk::Widget& root) const
  {
    applyThemeClass(root, _activePreset);
  }

  ThemeRegistrationToken ThemeCoordinator::registerToplevel(Gtk::Window& window)
  {
    _registeredWindows.push_back(&window);
    applyThemeClass(window, _activePreset);

    return ThemeRegistrationToken{this, &window};
  }

  void ThemeCoordinator::unregisterToplevel(Gtk::Window& window)
  {
    auto const position = std::ranges::find(_registeredWindows, &window);

    if (position != _registeredWindows.end())
    {
      _registeredWindows.erase(position);
    }

    if (!std::ranges::contains(_registeredWindows, &window))
    {
      removeThemeClass(window, _activePreset);
    }
  }

  void ThemeCoordinator::applyThemeClass(Gtk::Widget& widget, rt::ThemePresetId preset) const
  {
    removeThemeClass(widget, rt::ThemePresetId::Classic);
    removeThemeClass(widget, rt::ThemePresetId::Modern);
    widget.add_css_class(std::string{themeCssClass(preset)});
  }

  void ThemeCoordinator::removeThemeClass(Gtk::Widget& widget, rt::ThemePresetId preset) const
  {
    widget.remove_css_class(std::string{themeCssClass(preset)});
  }
} // namespace ao::gtk
