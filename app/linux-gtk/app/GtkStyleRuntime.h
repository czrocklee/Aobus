// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/utility/ScopedRegistration.h>

#include <giomm/dbusconnection.h>
#include <giomm/filemonitor.h>
#include <glib.h>
#include <glibmm/refptr.h>
#include <gtkmm/cssprovider.h>
#include <gtkmm/widget.h>
#include <sigc++/scoped_connection.h>
#include <sigc++/signal.h>

namespace ao::gtk
{
  class GtkStyleRuntime final
  {
  public:
    // Idempotent — safe to call multiple times.
    void initialize();

    // Stops callbacks and releases process-level GTK resources. Idempotent.
    void shutdown();

    // Full reload: syncGtkSettings + reloadGtkUserCss + reloadUserCss + signalRefreshed.
    void reload();

    sigc::signal<void()>& signalRefreshed();

    void addProviderForDisplayOf(Gtk::Widget& widget, Glib::RefPtr<Gtk::CssProvider> providerPtr, guint priority);
    void removeProviderForDisplayOf(Gtk::Widget& widget, Glib::RefPtr<Gtk::CssProvider> const& providerPtr);

    Glib::RefPtr<Gtk::CssProvider> const& appProvider() const;

    static GtkStyleRuntime& instance();

  private:
    GtkStyleRuntime() = default;

    void loadAppCss();
    void loadUserCss();
    void reloadUserCss();

    void syncGtkSettings();
    void reloadGtkUserCss();

    void startFileMonitors();
    void startGtkConfigMonitor();
    void startAobusConfigMonitor();
    void startDBusMonitor();

    bool _initialized = false;

    Glib::RefPtr<Gtk::CssProvider> _appProviderPtr;
    Glib::RefPtr<Gtk::CssProvider> _userProviderPtr;
    Glib::RefPtr<Gtk::CssProvider> _gtkUserCssProviderPtr;

    Glib::RefPtr<Gio::FileMonitor> _gtkConfigMonitorPtr;
    Glib::RefPtr<Gio::FileMonitor> _aobusConfigMonitorPtr;
    sigc::scoped_connection _gtkConfigMonitorConnection;
    sigc::scoped_connection _aobusConfigMonitorConnection;

    Glib::RefPtr<Gio::DBus::Connection> _dbusConnectionPtr;
    utility::ScopedRegistration _dbusSubscription;

    sigc::scoped_connection _reloadDebounceConnection;
    sigc::signal<void()> _refreshedSignal;
  };
} // namespace ao::gtk
