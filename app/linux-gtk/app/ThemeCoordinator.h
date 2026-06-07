// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/rt/StateTypes.h>

#include <gtkmm/widget.h>
#include <gtkmm/window.h>

#include <utility>
#include <vector>

namespace ao::gtk
{
  class AppConfig;
  class ThemeCoordinator;

  class [[nodiscard]] ThemeRegistrationToken final
  {
  public:
    ThemeRegistrationToken() = default;
    ThemeRegistrationToken(ThemeCoordinator* coordinator, Gtk::Window* window)
      : _coordinator{coordinator}, _window{window}
    {
    }

    ~ThemeRegistrationToken();

    ThemeRegistrationToken(ThemeRegistrationToken const&) = delete;
    ThemeRegistrationToken& operator=(ThemeRegistrationToken const&) = delete;

    ThemeRegistrationToken(ThemeRegistrationToken&& other) noexcept
      : _coordinator{std::exchange(other._coordinator, nullptr)}, _window{std::exchange(other._window, nullptr)}
    {
    }

    ThemeRegistrationToken& operator=(ThemeRegistrationToken&& other) noexcept
    {
      if (this != &other)
      {
        reset();
        _coordinator = std::exchange(other._coordinator, nullptr);
        _window = std::exchange(other._window, nullptr);
      }

      return *this;
    }

    void reset();

  private:
    ThemeCoordinator* _coordinator = nullptr;
    Gtk::Window* _window = nullptr;
  };

  class ThemeCoordinator final
  {
  public:
    void load(AppConfig const& config);
    void save(AppConfig& config) const;

    void setTheme(rt::ThemePresetId preset);
    rt::ThemePresetId activeTheme() const noexcept;

    void applyTo(Gtk::Widget& root) const;

    ThemeRegistrationToken registerToplevel(Gtk::Window& window);

  private:
    friend class ThemeRegistrationToken;

    rt::ThemePresetId _activePreset = rt::ThemePresetId::Classic;
    std::vector<Gtk::Window*> _registeredWindows;

    void unregisterToplevel(Gtk::Window& window);

    void applyThemeClass(Gtk::Widget& widget, rt::ThemePresetId preset) const;
    void removeThemeClass(Gtk::Widget& widget, rt::ThemePresetId preset) const;
  };
} // namespace ao::gtk
