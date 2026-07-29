// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "GtkApplicationTestSupport.h"

#include <gio/gio.h>
#include <glib.h>
#include <glibmm/main.h>
#include <gtkmm/application.h>
#include <gtkmm/widget.h>
#include <gtkmm/window.h>

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <string_view>
#include <thread>

namespace ao::gtk::test
{
  namespace
  {
    void installGtkTestLogHandler()
    {
      static bool installed = false;

      if (installed)
      {
        return;
      }

      ::g_log_set_writer_func(
        [](GLogLevelFlags logLevel, GLogField const* fields, gsize nFields, gpointer) -> GLogWriterOutput
        {
          for (gsize i = 0; i < nFields; ++i)
          {
            if (std::string_view{fields[i].key} != "MESSAGE")
            {
              continue;
            }

            auto message = std::string_view{};

            if (fields[i].length < 0)
            {
              message = static_cast<char const*>(fields[i].value);
            }
            else
            {
              message =
                std::string_view{static_cast<char const*>(fields[i].value), static_cast<std::size_t>(fields[i].length)};
            }

            if ((message.contains("Finalizing ") && message.contains("has children left")) ||
                message.contains(
                  "New application windows must be added after the GApplication::startup signal has been emitted"))
            {
              return G_LOG_WRITER_HANDLED;
            }
          }

          return ::g_log_writer_default(logLevel, fields, nFields, nullptr);
        },
        nullptr,
        nullptr);
      installed = true;
    }
  } // namespace

  Glib::RefPtr<Gtk::Application> ensureGtkApplication()
  {
    installGtkTestLogHandler();

    if (auto gioAppPtr = Gio::Application::get_default(); gioAppPtr)
    {
      if (auto gtkAppPtr = std::dynamic_pointer_cast<Gtk::Application>(gioAppPtr); gtkAppPtr)
      {
        return gtkAppPtr;
      }
    }

    return Gtk::Application::create("io.github.aobus.test", Gio::Application::Flags::NON_UNIQUE);
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

  bool pumpGtkEventsUntil(std::function<bool()> const& predicate, std::chrono::milliseconds const timeout)
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
