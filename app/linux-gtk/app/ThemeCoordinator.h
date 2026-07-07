// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/rt/AppPrefsState.h>

#include <gtkmm/widget.h>
#include <gtkmm/window.h>

#include <memory>
#include <utility>
#include <vector>

namespace ao::gtk
{
  class AppConfig;
  class ThemeCoordinator;
  struct ThemeWindowRegistration;

  class [[nodiscard]] ThemeRegistrationToken final
  {
  public:
    ThemeRegistrationToken() = default;

    ~ThemeRegistrationToken();

    ThemeRegistrationToken(ThemeRegistrationToken const&) = delete;
    ThemeRegistrationToken& operator=(ThemeRegistrationToken const&) = delete;

    ThemeRegistrationToken(ThemeRegistrationToken&& other) noexcept
      : _registrationPtr{std::move(other._registrationPtr)}
    {
    }

    ThemeRegistrationToken& operator=(ThemeRegistrationToken&& other) noexcept
    {
      if (this != &other)
      {
        reset();
        _registrationPtr = std::move(other._registrationPtr);
      }

      return *this;
    }

    void reset();

  private:
    friend class ThemeCoordinator;

    explicit ThemeRegistrationToken(std::shared_ptr<ThemeWindowRegistration> registrationPtr)
      : _registrationPtr{std::move(registrationPtr)}
    {
    }

    std::shared_ptr<ThemeWindowRegistration> _registrationPtr;
  };

  class ThemeCoordinator final
  {
  public:
    ThemeCoordinator() = default;
    ~ThemeCoordinator();

    ThemeCoordinator(ThemeCoordinator const&) = delete;
    ThemeCoordinator& operator=(ThemeCoordinator const&) = delete;
    ThemeCoordinator(ThemeCoordinator&&) = delete;
    ThemeCoordinator& operator=(ThemeCoordinator&&) = delete;

    void load(AppConfig const& config);
    void save(AppConfig& config) const;

    void setTheme(rt::ThemePresetId preset);
    rt::ThemePresetId activeTheme() const noexcept;

    void applyTo(Gtk::Widget& root) const;

    ThemeRegistrationToken registerToplevel(Gtk::Window& window);

  private:
    friend class ThemeRegistrationToken;

    rt::ThemePresetId _activePreset = rt::ThemePresetId::Classic;
    std::vector<std::shared_ptr<ThemeWindowRegistration>> _registeredWindows;

    void unregisterToplevel(std::shared_ptr<ThemeWindowRegistration> const& registrationPtr);
    void pruneExpiredRegistrations();

    void applyThemeClass(Gtk::Widget& widget, rt::ThemePresetId preset) const;
    void removeThemeClass(Gtk::Widget& widget, rt::ThemePresetId preset) const;
  };
} // namespace ao::gtk
