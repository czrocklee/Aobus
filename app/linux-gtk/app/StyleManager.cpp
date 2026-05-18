// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "app/StyleManager.h"
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

#include <cstdint>
#include <filesystem>

namespace ao::gtk
{
  namespace
  {
    constexpr std::uint32_t kReloadDebounceMs = 150;

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

  void StyleManager::initialize()
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

  void StyleManager::reload()
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
      kReloadDebounceMs);
  }

  sigc::signal<void()>& StyleManager::signalRefreshed()
  {
    return _refreshedSignal;
  }

  void StyleManager::registerWidgetProvider(Gtk::Widget& widget,
                                            Glib::RefPtr<Gtk::CssProvider> provider,
                                            guint priority)
  {
    Gtk::StyleContext::add_provider_for_display(widget.get_display(), provider, priority);
  }

  void StyleManager::unregisterWidgetProvider(Gtk::Widget& widget, Glib::RefPtr<Gtk::CssProvider> const& provider)
  {
    Gtk::StyleContext::remove_provider_for_display(widget.get_display(), provider);
  }

  Glib::RefPtr<Gtk::CssProvider> const& StyleManager::appProvider() const
  {
    return _appProvider;
  }

  StyleManager& StyleManager::instance()
  {
    static StyleManager manager;
    return manager;
  }

  // --------------------------------------------------------------------------
  // Private
  // --------------------------------------------------------------------------

  void StyleManager::loadAppCss()
  {
    auto const display = Gdk::Display::get_default();

    if (display == nullptr)
    {
      return;
    }

    // Load CSS from GResource
    auto appCss = Glib::ustring{};

    try
    {
      auto const data = Gio::Resource::lookup_data_global("/org/aobus/app.css");
      gsize size = 0;
      auto const* const buf = static_cast<char const*>(data->get_data(size));
      appCss = std::string{buf, size};
    }
    catch (Glib::Error const& err)
    {
      APP_LOG_ERROR("StyleManager: Failed to load app.css from GResource: {}", err.what());
    }

    _appProvider = Gtk::CssProvider::create();
    _appProvider->load_from_data(appCss);
    Gtk::StyleContext::add_provider_for_display(display, _appProvider, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  }

  void StyleManager::loadUserCss()
  {
    auto const display = Gdk::Display::get_default();

    if (display == nullptr)
    {
      return;
    }

    auto const userCssPath = std::filesystem::path{Glib::get_user_config_dir()} / "aobus" / "user.css";

    if (!std::filesystem::exists(userCssPath))
    {
      return;
    }

    _userProvider = Gtk::CssProvider::create();

    try
    {
      _userProvider->load_from_path(userCssPath.string());
      Gtk::StyleContext::add_provider_for_display(display, _userProvider, GTK_STYLE_PROVIDER_PRIORITY_USER);
    }
    catch (Glib::Error const& err)
    {
      APP_LOG_ERROR("StyleManager: Failed to load user.css: {}", err.what());
    }
  }

  void StyleManager::reloadUserCss()
  {
    auto const display = Gdk::Display::get_default();

    if (display == nullptr)
    {
      return;
    }

    if (_userProvider)
    {
      Gtk::StyleContext::remove_provider_for_display(display, _userProvider);
    }

    auto const userCssPath = std::filesystem::path{Glib::get_user_config_dir()} / "aobus" / "user.css";

    if (!std::filesystem::exists(userCssPath))
    {
      _userProvider.reset();
      return;
    }

    _userProvider = Gtk::CssProvider::create();

    try
    {
      _userProvider->load_from_path(userCssPath.string());
      Gtk::StyleContext::add_provider_for_display(display, _userProvider, GTK_STYLE_PROVIDER_PRIORITY_USER);
    }
    catch (Glib::Error const& err)
    {
      APP_LOG_ERROR("StyleManager: Failed to reload user.css: {}", err.what());
    }
  }

  void StyleManager::syncGtkSettings()
  {
    auto const settingsPath = std::filesystem::path{Glib::get_user_config_dir()} / "gtk-4.0" / "settings.ini";

    if (!std::filesystem::exists(settingsPath))
    {
      return;
    }

    try
    {
      auto const keyfile = Glib::KeyFile::create();
      keyfile->load_from_file(settingsPath.string());

      if (schemaExists("org.gnome.desktop.interface"))
      {
        auto const gsettings = Gio::Settings::create("org.gnome.desktop.interface");

        if (keyfile->has_key("Settings", "gtk-theme-name"))
        {
          gsettings->set_string("gtk-theme-name", keyfile->get_string("Settings", "gtk-theme-name"));
        }

        if (keyfile->has_key("Settings", "gtk-application-prefer-dark-theme"))
        {
          gsettings->set_boolean(
            "gtk-application-prefer-dark-theme", keyfile->get_boolean("Settings", "gtk-application-prefer-dark-theme"));
        }
      }
      else
      {
        auto const settings = Gtk::Settings::get_default();

        if (keyfile->has_key("Settings", "gtk-theme-name"))
        {
          settings->property_gtk_theme_name().set_value(keyfile->get_string("Settings", "gtk-theme-name"));
        }

        if (keyfile->has_key("Settings", "gtk-application-prefer-dark-theme"))
        {
          settings->property_gtk_application_prefer_dark_theme().set_value(
            keyfile->get_boolean("Settings", "gtk-application-prefer-dark-theme"));
        }
      }
    }
    catch (Glib::Error const& err)
    {
      APP_LOG_DEBUG("StyleManager: Could not read settings.ini: {}", err.what());
    }
  }

  void StyleManager::reloadGtkUserCss()
  {
    auto const display = Gdk::Display::get_default();

    if (display == nullptr)
    {
      return;
    }

    if (_gtkUserCssProvider)
    {
      Gtk::StyleContext::remove_provider_for_display(display, _gtkUserCssProvider);
    }

    auto const cssPath = std::filesystem::path{Glib::get_user_config_dir()} / "gtk-4.0" / "gtk.css";

    if (!std::filesystem::exists(cssPath))
    {
      _gtkUserCssProvider.reset();
      return;
    }

    _gtkUserCssProvider = Gtk::CssProvider::create();

    try
    {
      _gtkUserCssProvider->load_from_path(cssPath.string());
      Gtk::StyleContext::add_provider_for_display(display, _gtkUserCssProvider, GTK_STYLE_PROVIDER_PRIORITY_USER);
    }
    catch (Glib::Error const& err)
    {
      APP_LOG_ERROR("StyleManager: Failed to reload gtk.css: {}", err.what());
    }
  }

  void StyleManager::setupFileMonitors()
  {
    {
      auto const configDir = std::filesystem::path{Glib::get_user_config_dir()} / "gtk-4.0";
      auto const configFile = Gio::File::create_for_path(configDir.string());

      _gtkConfigMonitor = configFile->monitor_directory();
      _gtkConfigMonitor->signal_changed().connect(
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
              APP_LOG_DEBUG("StyleManager: gtk-4.0 change detected ({}), scheduling reload...", name);
              reload();
            }
          }
        });
    }

    {
      auto const aobusDir = std::filesystem::path{Glib::get_user_config_dir()} / "aobus";

      std::filesystem::create_directories(aobusDir);

      auto const aobusFile = Gio::File::create_for_path(aobusDir.string());

      _aobusConfigMonitor = aobusFile->monitor_directory();
      _aobusConfigMonitor->signal_changed().connect(
        [this](Glib::RefPtr<Gio::File> const& file,
               Glib::RefPtr<Gio::File> const& /*otherFile*/,
               Gio::FileMonitor::Event event)
        {
          using Event = Gio::FileMonitor::Event;

          if (event == Event::CHANGED || event == Event::CREATED || event == Event::CHANGES_DONE_HINT)
          {
            if (file->get_basename() == "user.css")
            {
              APP_LOG_DEBUG("StyleManager: user.css change detected, reloading...");
              reloadUserCss();
              _refreshedSignal.emit();
            }
          }
        });
    }
  }

  // NOLINTNEXTLINE(readability-function-cognitive-complexity)
  void StyleManager::setupDBusMonitor()
  {
    try
    {
      if (auto bus = Gio::DBus::Connection::get_sync(Gio::DBus::BusType::SESSION); bus)
      {
        _dbusSubscriptionId = bus->signal_subscribe(
          [this](Glib::RefPtr<Gio::DBus::Connection> const& /*connection*/,
                 Glib::ustring const& /*sender*/,
                 Glib::ustring const& /*iface*/,
                 Glib::ustring const& /*signalName*/,
                 Glib::ustring const& /*path*/,
                 Glib::VariantContainerBase const& /*parameters*/)
          {
            APP_LOG_DEBUG("StyleManager: DBus theme change detected via Portal, reloading...");
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
      APP_LOG_WARN("StyleManager: Failed to subscribe to DBus theme signals: {}", ex.what());
    }
  }

  void StyleManager::setupSignalHandler()
  {
    // NOLINTBEGIN(misc-include-cleaner)
    _sigusr1SourceId = ::g_unix_signal_add(
      SIGUSR1,
      [](void* data) -> ::gboolean
      {
        APP_LOG_DEBUG("StyleManager: Received SIGUSR1, scheduling theme refresh...");
        static_cast<StyleManager*>(data)->reload();
        return TRUE;
      },
      this);
    // NOLINTEND(misc-include-cleaner)
  }
} // namespace ao::gtk
