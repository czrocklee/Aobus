// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "GtkApplicationTestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <giomm/application.h>
#include <glib.h>
#include <glibmm/main.h>
#include <gtkmm/application.h>
#include <gtkmm/widget.h>
#include <gtkmm/window.h>

#include <chrono>
#include <functional>
#include <memory>
#include <string_view>
#include <thread>

namespace ao::gtk::test
{
  bool isOwnedGtkSessionBus(char const* const address, char const* const ownershipAddress) noexcept
  {
    return address != nullptr && ownershipAddress != nullptr && std::string_view{address}.starts_with("unix:") &&
           std::string_view{address}.size() > 5 && std::string_view{address} == ownershipAddress;
  }

  void requireOwnedGtkSessionBus()
  {
    if (!isOwnedGtkSessionBus(::g_getenv("DBUS_SESSION_BUS_ADDRESS"), ::g_getenv("AOBUS_OWNED_GTK_BUS")))
    {
      SKIP("This test requires a portal-owned session bus; run ./ao test --gtk");
    }
  }

  Glib::RefPtr<Gtk::Application> ensureGtkApplication()
  {
    if (auto gioAppPtr = Gio::Application::get_default(); gioAppPtr)
    {
      if (auto gtkAppPtr = std::dynamic_pointer_cast<Gtk::Application>(gioAppPtr); gtkAppPtr)
      {
        return gtkAppPtr;
      }
    }

    return Gtk::Application::create("io.github.aobus.test", Gio::Application::Flags::NON_UNIQUE);
  }

  Glib::RefPtr<Gtk::Application> ensureRegisteredGtkApplication()
  {
    auto const appPtr = ensureGtkApplication();

    if (!appPtr->is_registered())
    {
      REQUIRE(appPtr->register_application());
    }

    return appPtr;
  }

  void drainGtkEvents()
  {
    auto contextPtr = Glib::MainContext::get_default();

    while (contextPtr->pending())
    {
      contextPtr->iteration(false);
    }
  }

  void drainGtkEventsFor(std::chrono::milliseconds const duration)
  {
    auto const deadline = std::chrono::steady_clock::now() + duration;
    auto contextPtr = Glib::MainContext::get_default();

    while (std::chrono::steady_clock::now() < deadline)
    {
      while (contextPtr->pending())
      {
        contextPtr->iteration(false);
      }

      std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }

    drainGtkEvents();
  }

  bool tryPumpGtkEventsUntil(std::function<bool()> const& predicate, std::chrono::milliseconds const timeout)
  {
    auto const deadline = std::chrono::steady_clock::now() + timeout;
    auto contextPtr = Glib::MainContext::get_default();

    while (std::chrono::steady_clock::now() < deadline)
    {
      while (contextPtr->pending())
      {
        contextPtr->iteration(false);
      }

      if (predicate())
      {
        return true;
      }

      std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }

    drainGtkEvents();
    return predicate();
  }

  struct GtkWindowFixture::State final
  {
    Glib::RefPtr<Gtk::Application> appPtr = ensureGtkApplication();
    Gtk::Window window;
    bool mounted = false;
  };

  GtkWindowFixture::GtkWindowFixture()
    : _statePtr{std::make_unique<State>()}
  {
  }

  GtkWindowFixture::~GtkWindowFixture()
  {
    if (_statePtr->mounted)
    {
      _statePtr->window.unset_child();
      drainGtkEvents();
    }
  }

  Gtk::Window& GtkWindowFixture::window()
  {
    return _statePtr->window;
  }

  void GtkWindowFixture::mount(Gtk::Widget& widget)
  {
    _statePtr->window.set_child(widget);
    _statePtr->mounted = true;
  }

  void GtkWindowFixture::present()
  {
    _statePtr->window.present();
    drain();
  }

  void GtkWindowFixture::unmount()
  {
    if (_statePtr->mounted)
    {
      _statePtr->window.unset_child();
      _statePtr->mounted = false;
    }

    drain();
  }

  void GtkWindowFixture::drain()
  {
    drainGtkEvents();
  }
} // namespace ao::gtk::test
