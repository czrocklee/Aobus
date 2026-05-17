// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "app/StyleManager.h"
#include <ao/utility/Log.h>

#include <gdkmm/display.h>
#include <gio/gsettingsschema.h>
#include <giomm/dbusconnection.h>
#include <giomm/file.h>
#include <giomm/filemonitor.h>
#include <giomm/settings.h>
#include <glib-unix.h>
#include <glib.h>
#include <glib/gmacros.h>
#include <glibmm/error.h>
#include <glibmm/keyfile.h>
#include <glibmm/main.h>
#include <glibmm/miscutils.h>
#include <glibmm/refptr.h>
#include <glibmm/variant.h>
#include <gtk/gtk.h>
#include <gtkmm/cssprovider.h>
#include <gtkmm/settings.h>
#include <gtkmm/stylecontext.h>
#include <gtkmm/widget.h>
#include <sigc++/signal.h>

#include <cstdint>
#include <filesystem>
#include <string_view>

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

    constexpr auto kAppCss = std::string_view{R"css(
      /* ===== Aobus Design Tokens ===== */
      :root {
        /* Box spacing */
        --ao-gap-none: 0px;
        --ao-gap-sm: 4px;
        --ao-gap-md: 6px;
        --ao-gap-lg: 8px;
        --ao-gap-xl: 12px;

        /* margin/padding */
        --ao-spacing-xs: 2px;
        --ao-spacing-sm: 4px;
        --ao-spacing-md: 6px;
        --ao-spacing-lg: 8px;
        --ao-spacing-xl: 12px;

        /* Border radius */
        --ao-radius-sm: 4px;
        --ao-radius-md: 8px;
        --ao-radius-full: 100px;

        /* Audio quality semantic colors */
        --ao-quality-perfect: #A855F7;
        --ao-quality-lossless: #10B981;
        --ao-quality-intervention: #F59E0B;
        --ao-quality-lossy: #6B7280;
        --ao-quality-clipped: #EF4444;

        /* Opacity */
        --ao-opacity-dim: 0.4;
        --ao-opacity-muted: 0.7;

        /* Transitions */
        --ao-transition-fast: 200ms;
        --ao-transition-normal: 250ms;

        /* Seek bar */
        --ao-seek-trough-height: 6px;
        --ao-seek-slider-size: 14px;

        /* Font sizes */
        --ao-font-size-sm: 0.85rem;
        --ao-font-size-md: 0.9rem;
      }

      /* ===== Component CSS ===== */

      /* StatusBar */
      .ao-status-bar {
        min-height: 24px;
        padding-top: 1px;
        padding-bottom: 1px;
      }

      /* StatusNotificationLabel */
      .ao-status-message { }

      /* AobusSoulWindow */
      .ao-soul-window { background-color: rgba(0, 0, 0, 0.85); }

      /* NowPlayingStatusLabel */
      .ao-nowplaying {
        transition: all 200ms ease-in-out;
        padding: 2px 12px;
        border-radius: 6px;
        color: @theme_fg_color;
        opacity: 0.85;
      }
      .ao-nowplaying:hover {
        background-color: alpha(@theme_selected_bg_color, 0.15);
        color: @theme_selected_bg_color;
        opacity: 1.0;
      }
      .ao-nowplaying:active {
        background-color: alpha(@theme_selected_bg_color, 0.25);
        opacity: 0.7;
      }

      .ao-clickable { }

      /* OutputSelector */
      .ao-device-row {
        padding: 6px 16px;
        transition: background 150ms ease;
      }
      .ao-menu-header {
        font-weight: 800;
        font-size: 0.75em;
        padding-top: 12px;
        padding-bottom: 4px;
        padding-left: 12px;
        padding-right: 12px;
        color: @theme_selected_bg_color;
        text-transform: uppercase;
        letter-spacing: 0.12em;
        opacity: 0.7;
      }
      listboxrow:first-child .ao-menu-header {
        padding-top: 8px;
      }
      .ao-output-selected-row {
        background-color: alpha(@theme_selected_bg_color, 0.15);
        border-left: 4px solid @theme_selected_bg_color;
      }
      .ao-output-selected-row label {
        color: @theme_selected_bg_color;
        font-weight: bold;
      }
      .ao-menu-description {
        font-size: 0.8em;
        opacity: 0.6;
      }
      .ao-output-logo { }
      .ao-rich-list { }

      /* PlaybackDetailsWidget */
      .ao-quality-perfect { color: #A855F7; }
      .ao-quality-lossless { color: #10B981; }
      .ao-quality-intervention { color: #F59E0B; }
      .ao-quality-lossy { color: #6B7280; }
      .ao-quality-clipped { color: #EF4444; }

      /* ShellLayoutController */
      .ao-inspector-handle {
        min-width: 14px;
        padding: 0;
        margin: 0;
        border: none;
        border-radius: 0;
        background: transparent;
        transition: background 0.2s;
      }
      .ao-inspector-handle:hover {
        background: alpha(currentColor, 0.08);
      }
      .ao-inspector-handle image {
        opacity: 0.4;
        transition: opacity 0.2s;
      }
      .ao-inspector-handle:hover image {
        opacity: 1.0;
      }
      .ao-tags-section {
        margin-top: 4px;
      }
      .ao-tag-chip {
        border-radius: 100px;
        padding: 4px 10px;
        font-size: 0.85rem;
        font-weight: 500;
        transition: all 0.2s ease;
      }
      togglebutton.ao-tag-chip {
        background: alpha(currentColor, 0.05);
        border: 1px solid transparent;
        color: alpha(currentColor, 0.7);
      }
      togglebutton.ao-tag-chip:checked {
        background: alpha(currentColor, 0.15);
        color: currentColor;
        border-color: alpha(currentColor, 0.1);
      }
      togglebutton.ao-tag-chip:hover {
        background: alpha(currentColor, 0.2);
      }
      .ao-tag-remove {
        min-width: 18px;
        min-height: 18px;
        padding: 0;
        margin-left: 4px;
        border-radius: 100px;
        background: transparent;
        border: none;
        opacity: 0.4;
        transition: opacity 0.2s;
      }
      .ao-tag-remove:hover {
        opacity: 1.0;
        background: alpha(@error_color, 0.1);
      }
      .ao-tags-entry {
        background: alpha(currentColor, 0.05);
        border: 1px solid transparent;
        border-radius: 8px;
        padding: 6px 12px;
        margin-top: 8px;
        transition: all 0.2s;
        font-size: 0.9rem;
      }
      .ao-tags-entry:focus {
        border-color: alpha(@accent_color, 0.5);
        background: alpha(currentColor, 0.08);
        box-shadow: none;
      }

      /* TrackColumnFactoryBuilder */
      columnview row.ao-playing-row {
        background-image: linear-gradient(to right,
          transparent 0%,
          alpha(@warning_bg_color, 0.2) var(--ao-title-x, 35%),
          transparent 100%
        );
        background-color: transparent;
        border-color: transparent;
        transition: background-image 1.0s ease-out;
      }
      .ao-playing-title {
        color: @theme_fg_color;
        font-weight: bold;
      }
      .ao-playing-dim { }
      columnview row {
        transition: all 450ms cubic-bezier(0.16, 1, 0.3, 1);
      }
      .ao-inline-editor-stack { min-height: 0; margin: 0; }
      .ao-inline-editor-label { border: 1px solid transparent; min-height: 0; }
      .ao-inline-editor-entry {
        background: @view_bg_color;
        border: 1px solid @accent_color;
        border-radius: 4px;
        padding: 0 6px;
        margin: 0;
        min-height: 0;
        box-shadow: none;
        font-weight: bold;
      }
      .ao-inline-editor-entry text { padding-top: 0; padding-bottom: 0; min-height: 0; }
      .ao-track-tags-cell { }

      /* SeekControl */
      .ao-seekbar { padding: var(--ao-spacing-sm) 0; }
      .ao-seekbar trough {
        min-height: var(--ao-seek-trough-height);
        border-radius: 3px;
        background: alpha(currentColor, 0.12);
      }
      .ao-seekbar highlight {
        background: @theme_selected_bg_color;
        border-radius: 3px;
      }
      .ao-seekbar slider {
        min-width: var(--ao-seek-slider-size);
        min-height: var(--ao-seek-slider-size);
        border-radius: 50%;
      }
    )css"};
  } // namespace

  void StyleManager::initialize()
  {
    if (_initialized)
    {
      return;
    }

    _initialized = true;

    loadAppCss();
    loadUserCss();
    syncGtkSettings();
    reloadGtkUserCss();
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

    _appProvider = Gtk::CssProvider::create();
    _appProvider->load_from_data(Glib::ustring{kAppCss.data(), kAppCss.size()});
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
            auto const name = file->get_basename();

            if (name == "settings.ini" || name == "gtk.css")
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
