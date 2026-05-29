// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <giomm/filemonitor.h>
#include <glib.h>
#include <glibmm/refptr.h>
#include <gtkmm/cssprovider.h>
#include <gtkmm/widget.h>
#include <sigc++/connection.h>
#include <sigc++/signal.h>

namespace ao::gtk
{
  class StyleManager final
  {
  public:
    // Idempotent — safe to call multiple times.
    void initialize();

    // Full reload: syncGtkSettings + reloadGtkUserCss + reloadUserCss + signalRefreshed.
    void reload();

    sigc::signal<void()>& signalRefreshed();

    void registerWidgetProvider(Gtk::Widget& widget, Glib::RefPtr<Gtk::CssProvider> providerPtr, guint priority);
    void unregisterWidgetProvider(Gtk::Widget& widget, Glib::RefPtr<Gtk::CssProvider> const& provider);

    Glib::RefPtr<Gtk::CssProvider> const& appProvider() const;

    static StyleManager& instance();

  private:
    StyleManager() = default;

    void loadAppCss();
    void loadUserCss();
    void reloadUserCss();

    void syncGtkSettings();
    void reloadGtkUserCss();

    void setupFileMonitors();
    void setupGtkConfigMonitor();
    void setupAobusConfigMonitor();
    void setupDBusMonitor();
    void setupSignalHandler();

    bool _initialized = false;

    Glib::RefPtr<Gtk::CssProvider> _appProviderPtr;
    Glib::RefPtr<Gtk::CssProvider> _userProviderPtr;
    Glib::RefPtr<Gtk::CssProvider> _gtkUserCssProviderPtr;

    Glib::RefPtr<Gio::FileMonitor> _gtkConfigMonitorPtr;
    Glib::RefPtr<Gio::FileMonitor> _aobusConfigMonitorPtr;

    guint _dbusSubscriptionId = 0;
    guint _sigusr1SourceId = 0;

    sigc::connection _reloadDebounceConnection;
    sigc::signal<void()> _refreshedSignal;
  };
}
