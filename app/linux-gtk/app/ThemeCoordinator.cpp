// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors
#include "app/ThemeCoordinator.h"

#include "app/AppConfig.h"
#include "app/ThemePreset.h"
#include <ao/rt/AppPrefsState.h>

#include <glib-object.h>
#include <glibmm/objectbase.h>
#include <gtkmm/widget.h>
#include <gtkmm/window.h>

#include <algorithm>
#include <memory>
#include <string>
#include <utility>

namespace ao::gtk
{
  struct ThemeWindowRegistration final
  {
    ThemeCoordinator* coordinator = nullptr;
    Gtk::Window* window = nullptr;
    bool weakRefActive = false;
  };

  namespace
  {
    void onRegisteredWindowFinalized(void* const data, GObject* /*whereTheObjectWas*/)
    {
      auto* const registration = static_cast<ThemeWindowRegistration*>(data);

      if (registration != nullptr)
      {
        registration->window = nullptr;
        registration->weakRefActive = false;
      }
    }

    GObject* gObjectFor(Gtk::Window& window) noexcept
    {
      return static_cast<Glib::ObjectBase&>(window).gobj();
    }

    void disconnectWindowWeakRef(ThemeWindowRegistration& registration)
    {
      if (registration.weakRefActive && registration.window != nullptr)
      {
        ::g_object_weak_unref(gObjectFor(*registration.window), onRegisteredWindowFinalized, &registration);
      }

      registration.weakRefActive = false;
    }

    bool registrationMatchesWindow(std::shared_ptr<ThemeWindowRegistration> const& registrationPtr,
                                   Gtk::Window const* const window)
    {
      return registrationPtr != nullptr && registrationPtr->window == window;
    }
  } // namespace

  ThemeRegistrationToken::~ThemeRegistrationToken()
  {
    reset();
  }

  void ThemeRegistrationToken::reset()
  {
    if (_registrationPtr == nullptr)
    {
      return;
    }

    if (_registrationPtr->coordinator != nullptr)
    {
      _registrationPtr->coordinator->unregisterToplevel(_registrationPtr);
    }
    else
    {
      disconnectWindowWeakRef(*_registrationPtr);
      _registrationPtr->window = nullptr;
    }

    _registrationPtr.reset();
  }

  ThemeCoordinator::~ThemeCoordinator()
  {
    for (auto const& registrationPtr : _registeredWindows)
    {
      if (registrationPtr != nullptr)
      {
        disconnectWindowWeakRef(*registrationPtr);
        registrationPtr->coordinator = nullptr;
        registrationPtr->window = nullptr;
      }
    }
  }

  void ThemeCoordinator::load(AppConfig const& config)
  {
    auto prefs = rt::AppPrefsState{};
    config.loadAppPrefs(prefs);
    _activePreset = rt::themePresetFromString(prefs.lastThemePreset);
  }

  void ThemeCoordinator::save(AppConfig& config) const
  {
    auto prefs = rt::AppPrefsState{};
    config.loadAppPrefs(prefs);
    prefs.lastThemePreset = std::string{rt::themePresetToString(_activePreset)};
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

    pruneExpiredRegistrations();

    for (auto const& registrationPtr : _registeredWindows)
    {
      if (auto* const window = registrationPtr->window; window != nullptr)
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
    pruneExpiredRegistrations();

    auto registrationPtr = std::make_shared<ThemeWindowRegistration>();
    registrationPtr->coordinator = this;
    registrationPtr->window = &window;
    ::g_object_weak_ref(gObjectFor(window), onRegisteredWindowFinalized, registrationPtr.get());
    registrationPtr->weakRefActive = true;

    _registeredWindows.push_back(registrationPtr);
    applyThemeClass(window, _activePreset);

    return ThemeRegistrationToken{std::move(registrationPtr)};
  }

  void ThemeCoordinator::unregisterToplevel(std::shared_ptr<ThemeWindowRegistration> const& registrationPtr)
  {
    if (registrationPtr == nullptr)
    {
      return;
    }

    auto* const window = registrationPtr->window;
    auto const position = std::ranges::find(_registeredWindows, registrationPtr);

    if (position != _registeredWindows.end())
    {
      _registeredWindows.erase(position);
    }

    if (window != nullptr)
    {
      bool const hasRemainingRegistration = std::ranges::any_of(
        _registeredWindows,
        [window](auto const& candidatePtr) { return registrationMatchesWindow(candidatePtr, window); });

      if (!hasRemainingRegistration)
      {
        removeThemeClass(*window, _activePreset);
      }
    }

    disconnectWindowWeakRef(*registrationPtr);
    registrationPtr->coordinator = nullptr;
    registrationPtr->window = nullptr;
  }

  void ThemeCoordinator::pruneExpiredRegistrations()
  {
    std::erase_if(_registeredWindows,
                  [](std::shared_ptr<ThemeWindowRegistration> const& registrationPtr)
                  {
                    if (registrationPtr == nullptr || registrationPtr->window == nullptr)
                    {
                      if (registrationPtr != nullptr)
                      {
                        registrationPtr->coordinator = nullptr;
                      }

                      return true;
                    }

                    return false;
                  });
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
