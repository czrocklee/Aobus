// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "app/GtkStyleRuntime.h"

#include <ao/utility/Log.h>

#include <gdkmm/display.h>
#include <gio/gsettingsschema.h>
#include <giomm/dbusconnection.h>
#include <giomm/file.h>
#include <giomm/filemonitor.h>
#include <giomm/resource.h>
#include <giomm/settings.h>
#include <glib-unix.h>
#include <glib.h>
#include <glib/gmacros.h>
#include <glibconfig.h>
#include <glibmm/error.h>
#include <glibmm/keyfile.h>
#include <glibmm/main.h>
#include <glibmm/miscutils.h>
#include <glibmm/refptr.h>
#include <glibmm/ustring.h>
#include <glibmm/variant.h>
#include <gtk/gtk.h>
#include <gtkmm/cssprovider.h>
#include <gtkmm/settings.h>
#include <gtkmm/stylecontext.h>
#include <gtkmm/widget.h>
#include <sigc++/signal.h>

#include <chrono>
#include <filesystem>

namespace ao::gtk
{
  namespace
  {
    constexpr auto kReloadDebounceInterval = std::chrono::milliseconds{150};

    bool schemaExists(char const* schemaId)
    {
      auto* const source = ::g_settings_schema_source_get_default();

      if (source == nullptr)
      {
        return false;
      }

      auto* const schema = ::g_settings_schema_source_lookup(source, schemaId, TRUE);

      if (schema == nullptr)
      {
        return false;
      }

      ::g_settings_schema_unref(schema);
      return true;
    }
  } // namespace

  void GtkStyleRuntime::initialize()
  {
    if (_initialized)
    {
      return;
    }

    _initialized = true;

    loadAppCss();
    syncGtkSettings();
    reloadGtkUserCss();
    loadUserCss();
    setupFileMonitors();
    setupDBusMonitor();
    setupSignalHandler();
  }

  void GtkStyleRuntime::reload()
  {
    _reloadDebounceConnection.disconnect();
    _reloadDebounceConnection = Glib::signal_timeout().connect(
      [this] -> bool
      {
        syncGtkSettings();
        reloadGtkUserCss();
        reloadUserCss();

        _refreshedSignal.emit();
        return false;
      },
      kReloadDebounceInterval.count());
  }

  sigc::signal<void()>& GtkStyleRuntime::signalRefreshed()
  {
    return _refreshedSignal;
  }

  void GtkStyleRuntime::addProviderForDisplayOf(Gtk::Widget& widget,
                                                Glib::RefPtr<Gtk::CssProvider> providerPtr,
                                                guint priority)
  {
    Gtk::StyleContext::add_provider_for_display(widget.get_display(), providerPtr, priority);
  }

  void GtkStyleRuntime::removeProviderForDisplayOf(Gtk::Widget& widget, Glib::RefPtr<Gtk::CssProvider> const& provider)
  {
    Gtk::StyleContext::remove_provider_for_display(widget.get_display(), provider);
  }

  Glib::RefPtr<Gtk::CssProvider> const& GtkStyleRuntime::appProvider() const
  {
    return _appProviderPtr;
  }

  GtkStyleRuntime& GtkStyleRuntime::instance()
  {
    static GtkStyleRuntime manager;
    return manager;
  }

  // --------------------------------------------------------------------------
  // Private
  // --------------------------------------------------------------------------

  void GtkStyleRuntime::loadAppCss()
  {
    auto const displayPtr = Gdk::Display::get_default();

    if (displayPtr == nullptr)
    {
      return;
    }

    _appProviderPtr = Gtk::CssProvider::create();

    try
    {
      _appProviderPtr->load_from_resource("/org/aobus/app.css");
    }
    catch (Glib::Error const& err)
    {
      APP_LOG_ERROR("GtkStyleRuntime: Failed to load app.css from GResource: {}", err.what());
    }

    Gtk::StyleContext::add_provider_for_display(displayPtr, _appProviderPtr, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  }

  void GtkStyleRuntime::loadUserCss()
  {
    auto const displayPtr = Gdk::Display::get_default();

    if (displayPtr == nullptr)
    {
      return;
    }

    auto const userCssPath = std::filesystem::path{Glib::get_user_config_dir()} / "aobus" / "user.css";

    if (!std::filesystem::exists(userCssPath))
    {
      return;
    }

    _userProviderPtr = Gtk::CssProvider::create();

    try
    {
      _userProviderPtr->load_from_path(userCssPath.string());
      Gtk::StyleContext::add_provider_for_display(displayPtr, _userProviderPtr, GTK_STYLE_PROVIDER_PRIORITY_USER);
    }
    catch (Glib::Error const& err)
    {
      APP_LOG_ERROR("GtkStyleRuntime: Failed to load user.css: {}", err.what());
    }
  }

  void GtkStyleRuntime::reloadUserCss()
  {
    auto const displayPtr = Gdk::Display::get_default();

    if (displayPtr == nullptr)
    {
      return;
    }

    if (_userProviderPtr)
    {
      Gtk::StyleContext::remove_provider_for_display(displayPtr, _userProviderPtr);
    }

    auto const userCssPath = std::filesystem::path{Glib::get_user_config_dir()} / "aobus" / "user.css";

    if (!std::filesystem::exists(userCssPath))
    {
      _userProviderPtr.reset();
      return;
    }

    _userProviderPtr = Gtk::CssProvider::create();

    try
    {
      _userProviderPtr->load_from_path(userCssPath.string());
      Gtk::StyleContext::add_provider_for_display(displayPtr, _userProviderPtr, GTK_STYLE_PROVIDER_PRIORITY_USER);
    }
    catch (Glib::Error const& err)
    {
      APP_LOG_ERROR("GtkStyleRuntime: Failed to reload user.css: {}", err.what());
    }
  }

  void GtkStyleRuntime::syncGtkSettings()
  {
    auto const settingsPath = std::filesystem::path{Glib::get_user_config_dir()} / "gtk-4.0" / "settings.ini";

    if (!std::filesystem::exists(settingsPath))
    {
      return;
    }

    try
    {
      auto const keyfilePtr = Glib::KeyFile::create();
      keyfilePtr->load_from_file(settingsPath.string());

      if (schemaExists("org.gnome.desktop.interface"))
      {
        auto const gsettingsPtr = Gio::Settings::create("org.gnome.desktop.interface");

        auto* const source = ::g_settings_schema_source_get_default();
        auto* const schema = ::g_settings_schema_source_lookup(source, "org.gnome.desktop.interface", TRUE);

        auto const hasSchemaKey = [schema](char const* key)
        { return schema != nullptr && ::g_settings_schema_has_key(schema, key); };

        if (keyfilePtr->has_key("Settings", "gtk-theme-name") && hasSchemaKey("gtk-theme-name"))
        {
          gsettingsPtr->set_string("gtk-theme-name", keyfilePtr->get_string("Settings", "gtk-theme-name"));
        }

        if (keyfilePtr->has_key("Settings", "gtk-application-prefer-dark-theme") &&
            hasSchemaKey("gtk-application-prefer-dark-theme"))
        {
          gsettingsPtr->set_boolean("gtk-application-prefer-dark-theme",
                                    keyfilePtr->get_boolean("Settings", "gtk-application-prefer-dark-theme"));
        }

        if (schema != nullptr)
        {
          ::g_settings_schema_unref(schema);
        }
      }
      else
      {
        auto const settingsPtr = Gtk::Settings::get_default();

        if (keyfilePtr->has_key("Settings", "gtk-theme-name"))
        {
          settingsPtr->property_gtk_theme_name().set_value(keyfilePtr->get_string("Settings", "gtk-theme-name"));
        }

        if (keyfilePtr->has_key("Settings", "gtk-application-prefer-dark-theme"))
        {
          settingsPtr->property_gtk_application_prefer_dark_theme().set_value(
            keyfilePtr->get_boolean("Settings", "gtk-application-prefer-dark-theme"));
        }
      }
    }
    catch (Glib::Error const& err)
    {
      APP_LOG_DEBUG("GtkStyleRuntime: Could not read settings.ini: {}", err.what());
    }
  }

  void GtkStyleRuntime::reloadGtkUserCss()
  {
    auto const displayPtr = Gdk::Display::get_default();

    if (displayPtr == nullptr)
    {
      return;
    }

    if (_gtkUserCssProviderPtr)
    {
      Gtk::StyleContext::remove_provider_for_display(displayPtr, _gtkUserCssProviderPtr);
    }

    auto const cssPath = std::filesystem::path{Glib::get_user_config_dir()} / "gtk-4.0" / "gtk.css";

    if (!std::filesystem::exists(cssPath))
    {
      _gtkUserCssProviderPtr.reset();
      return;
    }

    _gtkUserCssProviderPtr = Gtk::CssProvider::create();

    try
    {
      _gtkUserCssProviderPtr->load_from_path(cssPath.string());
      Gtk::StyleContext::add_provider_for_display(displayPtr, _gtkUserCssProviderPtr, GTK_STYLE_PROVIDER_PRIORITY_USER);
    }
    catch (Glib::Error const& err)
    {
      APP_LOG_ERROR("GtkStyleRuntime: Failed to reload gtk.css: {}", err.what());
    }
  }

  void GtkStyleRuntime::setupGtkConfigMonitor()
  {
    auto const configDir = std::filesystem::path{Glib::get_user_config_dir()} / "gtk-4.0";
    auto const configFilePtr = Gio::File::create_for_path(configDir.string());

    _gtkConfigMonitorPtr = configFilePtr->monitor_directory();
    _gtkConfigMonitorPtr->signal_changed().connect(
      [this](Glib::RefPtr<Gio::File> const& file,
             Glib::RefPtr<Gio::File> const& /*otherFile*/,
             Gio::FileMonitor::Event event)
      {
        using Event = Gio::FileMonitor::Event;

        if (event == Event::CHANGED || event == Event::CREATED || event == Event::DELETED ||
            event == Event::CHANGES_DONE_HINT)
        {
          if (auto const name = file->get_basename(); name == "settings.ini" || name == "gtk.css")
          {
            APP_LOG_DEBUG("GtkStyleRuntime: gtk-4.0 change detected ({}), scheduling reload...", name);
            reload();
          }
        }
      });
  }

  void GtkStyleRuntime::setupAobusConfigMonitor()
  {
    auto const aobusDir = std::filesystem::path{Glib::get_user_config_dir()} / "aobus";

    std::filesystem::create_directories(aobusDir);

    auto const aobusFilePtr = Gio::File::create_for_path(aobusDir.string());

    _aobusConfigMonitorPtr = aobusFilePtr->monitor_directory();
    _aobusConfigMonitorPtr->signal_changed().connect(
      [this](Glib::RefPtr<Gio::File> const& file,
             Glib::RefPtr<Gio::File> const& /*otherFile*/,
             Gio::FileMonitor::Event event)
      {
        using Event = Gio::FileMonitor::Event;

        if (event == Event::CHANGED || event == Event::CREATED || event == Event::CHANGES_DONE_HINT)
        {
          if (file->get_basename() == "user.css")
          {
            APP_LOG_DEBUG("GtkStyleRuntime: user.css change detected, reloading...");
            reloadUserCss();
            _refreshedSignal.emit();
          }
        }
      });
  }

  void GtkStyleRuntime::setupFileMonitors()
  {
    setupGtkConfigMonitor();
    setupAobusConfigMonitor();
  }

  void GtkStyleRuntime::setupDBusMonitor()
  {
    try
    {
      if (auto busPtr = Gio::DBus::Connection::get_sync(Gio::DBus::BusType::SESSION); busPtr)
      {
        _dbusSubscriptionId = busPtr->signal_subscribe(
          [this](Glib::RefPtr<Gio::DBus::Connection> const& /*connection*/,
                 Glib::ustring const& /*sender*/,
                 Glib::ustring const& /*iface*/,
                 Glib::ustring const& /*signalName*/,
                 Glib::ustring const& /*path*/,
                 Glib::VariantContainerBase const& /*parameters*/)
          {
            APP_LOG_DEBUG("GtkStyleRuntime: DBus theme change detected via Portal, reloading...");
            reload();
          },
          "org.freedesktop.portal.Desktop",
          "org.freedesktop.portal.Settings",
          "SettingChanged",
          "/org/freedesktop/portal/desktop");
      }
    }
    catch (Glib::Error const& ex)
    {
      APP_LOG_WARN("GtkStyleRuntime: Failed to subscribe to DBus theme signals: {}", ex.what());
    }
  }

  void GtkStyleRuntime::setupSignalHandler()
  {
    _sigusr1SourceId = ::g_unix_signal_add(
      SIGUSR1,
      [](void* data) -> ::gboolean
      {
        APP_LOG_DEBUG("GtkStyleRuntime: Received SIGUSR1, scheduling theme refresh...");
        static_cast<GtkStyleRuntime*>(data)->reload();
        return TRUE;
      },
      this);
  }
} // namespace ao::gtk
