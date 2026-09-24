// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <glibmm/refptr.h>

#include <chrono>
#include <functional>
#include <memory>

namespace Gtk
{
  class Application;
  class Widget;
  class Window;
} // namespace Gtk

namespace ao::gtk::test
{
  bool isOwnedGtkSessionBus(char const* address, char const* ownershipAddress) noexcept;
  void requireOwnedGtkSessionBus();
  Glib::RefPtr<Gtk::Application> ensureGtkApplication();
  Glib::RefPtr<Gtk::Application> ensureRegisteredGtkApplication();
  void drainGtkEvents();
  void drainGtkEventsFor(std::chrono::milliseconds duration);
  bool tryPumpGtkEventsUntil(std::function<bool()> const& predicate,
                             std::chrono::milliseconds timeout = std::chrono::seconds{10});

  class GtkWindowFixture final
  {
  public:
    GtkWindowFixture();
    ~GtkWindowFixture();

    GtkWindowFixture(GtkWindowFixture const&) = delete;
    GtkWindowFixture& operator=(GtkWindowFixture const&) = delete;
    GtkWindowFixture(GtkWindowFixture&&) = delete;
    GtkWindowFixture& operator=(GtkWindowFixture&&) = delete;

    Gtk::Window& window();
    void mount(Gtk::Widget& widget);
    void present();
    void unmount();
    void drain();

  private:
    struct State;
    std::unique_ptr<State> _statePtr;
  };
} // namespace ao::gtk::test
