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

    void syncSettingsFromFile(std::string const& settingsPath)
    {
      try
      {
        auto const keyfile = Glib::KeyFile::create();
        keyfile->load_from_file(settingsPath);

        if (schemaExists("org.gnome.desktop.interface"))
        {
          auto const gsettings = Gio::Settings::create("org.gnome.desktop.interface");

          if (keyfile->has_key("Settings", "gtk-theme-name"))
          {
            gsettings->set_string("gtk-theme-name", keyfile->get_string("Settings", "gtk-theme-name"));
          }

          if (keyfile->has_key("Settings", "gtk-application-prefer-dark-theme"))
          {
            gsettings->set_boolean("gtk-application-prefer-dark-theme",
                                   keyfile->get_boolean("Settings", "gtk-application-prefer-dark-theme"));
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
        APP_LOG_DEBUG("ThemeBus: Could not read settings.ini: {}", err.what());
      }
    }

    void reloadUserCss(std::string const& cssPath, Glib::RefPtr<Gtk::CssProvider>& s_userCssProvider)
    {
      auto const display = Gdk::Display::get_default();
      if (display == nullptr)
      {
        return;
      }

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
  }

  void emitThemeRefresh()
  {
    static auto debounce_connection = sigc::connection{};
    static Glib::RefPtr<Gtk::CssProvider> s_userCssProvider;

    debounce_connection.disconnect();
    debounce_connection = Glib::signal_timeout().connect(
      []() -> bool
      {
        auto const configDir = std::string(Glib::get_user_config_dir()) + "/gtk-4.0";
        auto const settingsPath = configDir + "/settings.ini";
        auto const cssPath = configDir + "/gtk.css";

        syncSettingsFromFile(settingsPath);
        reloadUserCss(cssPath, s_userCssProvider);

        signalThemeRefresh().emit();
        return false;
      },
      150); // NOLINT(readability-magic-numbers)
  }
}
