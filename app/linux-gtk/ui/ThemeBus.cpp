#include "ThemeBus.h"
#include <ao/utility/Log.h>
#include <gdkmm/display.h>
#include <gio/gio.h>
#include <giomm/settings.h>
#include <glibmm/keyfile.h>
#include <glibmm/main.h>
#include <glibmm/miscutils.h>
#include <gtkmm/cssprovider.h>
#include <gtkmm/settings.h>
#include <gtkmm/stylecontext.h>

namespace ao::gtk
{
  sigc::signal<void()>& signalThemeRefresh()
  {
    static sigc::signal<void()> signal;
    return signal;
  }

  namespace
  {
    bool schemaExists(char const* schemaId)
    {
      auto* source = g_settings_schema_source_get_default();
      if (source == nullptr) return false;

      auto* schema = g_settings_schema_source_lookup(source, schemaId, TRUE);
      if (schema == nullptr) return false;

      g_settings_schema_unref(schema);
      return true;
    }
  }

  void emitThemeRefresh()
  {
    static sigc::connection debounce_connection;
    static Glib::RefPtr<Gtk::CssProvider> s_userCssProvider;

    debounce_connection.disconnect();
    debounce_connection = Glib::signal_timeout().connect(
      []() -> bool
      {
        auto const configDir = std::string(Glib::get_user_config_dir()) + "/gtk-4.0";
        auto const settingsPath = configDir + "/settings.ini";
        auto const cssPath = configDir + "/gtk.css";

        // 1. Read settings.ini and push values where they'll take effect.
        try
        {
          auto keyfile = Glib::KeyFile::create();
          keyfile->load_from_file(settingsPath);

          if (schemaExists("org.gnome.desktop.interface"))
          {
            // GSettings is available — write there so GTK's internal
            // GSettings binding triggers theme reload automatically.
            auto gsettings = Gio::Settings::create("org.gnome.desktop.interface");

            if (keyfile->has_key("Settings", "gtk-theme-name"))
              gsettings->set_string("gtk-theme-name", keyfile->get_string("Settings", "gtk-theme-name"));
            if (keyfile->has_key("Settings", "gtk-application-prefer-dark-theme"))
              gsettings->set_boolean("gtk-application-prefer-dark-theme",
                                     keyfile->get_boolean("Settings", "gtk-application-prefer-dark-theme"));
          }
          else
          {
            // No GSettings — set Gtk::Settings directly (safe because
            // there is no GSettings backend to push the old value back).
            auto settings = Gtk::Settings::get_default();

            if (keyfile->has_key("Settings", "gtk-theme-name"))
              settings->property_gtk_theme_name().set_value(keyfile->get_string("Settings", "gtk-theme-name"));
            if (keyfile->has_key("Settings", "gtk-application-prefer-dark-theme"))
              settings->property_gtk_application_prefer_dark_theme().set_value(
                keyfile->get_boolean("Settings", "gtk-application-prefer-dark-theme"));
          }
        }
        catch (Glib::Error const&)
        {
        }

        // 2. Force-reload gtk.css at USER priority to pick up Stylix color
        //    changes even when the theme name hasn't changed.
        if (auto display = Gdk::Display::get_default(); display)
        {
          if (s_userCssProvider)
          {
            Gtk::StyleContext::remove_provider_for_display(display, s_userCssProvider);
          }

          s_userCssProvider = Gtk::CssProvider::create();
          try
          {
            s_userCssProvider->load_from_path(cssPath);
            Gtk::StyleContext::add_provider_for_display(display, s_userCssProvider, GTK_STYLE_PROVIDER_PRIORITY_USER);
          }
          catch (Glib::Error const& err)
          {
            APP_LOG_ERROR("ThemeBus: Failed to reload user CSS: {}", err.what());
          }
        }

        // 3. Notify widgets to reload their own CSS providers.
        signalThemeRefresh().emit();
        return false;
      },
      150);
  }
}
