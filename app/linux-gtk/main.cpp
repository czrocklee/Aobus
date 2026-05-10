// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "MainWindow.h"
#include "TrackRowDataProvider.h"
#include "ui/GtkControlExecutor.h"
#include "ui/ThemeBus.h"
#include <ao/utility/Log.h>
#include <common/AppConfig.h>
#include <giomm/dbusconnection.h>
#include <giomm/dbusproxy.h>
#include <giomm/file.h>
#include <giomm/filemonitor.h>
#include <runtime/AppSession.h>

#include <ao/AppVersion.h>

#include <gsl-lite/gsl-lite.hpp>
#include <gtkmm.h>
#include <gtkmm/aboutdialog.h>

#include <glib-unix.h>

#include <CLI/CLI.hpp>

#include "ui/SessionPersistence.h"

namespace
{
  std::filesystem::path resolveLibraryPath()
  {
    try
    {
      auto config = ao::app::AppConfig::load();
      auto const& path = config.sessionState().lastLibraryPath;
      if (!path.empty())
      {
        return std::filesystem::path{path};
      }
    }
    catch (...)
    {
    }

    auto emptyPath = std::filesystem::temp_directory_path() / "aobus-empty";
    std::filesystem::create_directories(emptyPath);
    return emptyPath;
  }

  Glib::RefPtr<ao::gtk::MainWindow> createWindow(Gtk::Application& app, std::filesystem::path libraryPath)
  {
    auto executor = std::make_shared<ao::gtk::GtkControlExecutor>();
    auto persistence = std::make_shared<ao::gtk::SessionPersistence>();

    auto appSession = std::make_unique<ao::app::AppSession>(ao::app::AppSessionDependencies{
      .executor = std::move(executor), .libraryRoot = std::move(libraryPath), .persistence = persistence});

    auto window =
      Glib::make_refptr_for_instance<ao::gtk::MainWindow>(new ao::gtk::MainWindow(*appSession, persistence));

    // Store AppSession alongside window (lifetime tied to window via pointer)
    window->set_data("app-session",
                     new std::unique_ptr<ao::app::AppSession>(std::move(appSession)),
                     [](void* data) { delete static_cast<std::unique_ptr<ao::app::AppSession>*>(data); });

    window->initializeSession();

    app.add_window(*window);
    window->present();

    return window;
  }
}

int main(int argc, char* argv[])
{
  CLI::App cliApp{"Aobus Music Library"};
  cliApp.allow_extras(); // Allow GTK specific arguments

  auto logLevel = ao::log::LogLevel::Info;

  // Map strings to LogLevel enum for CLI11
  std::map<std::string, ao::log::LogLevel> logMapping{{"trace", ao::log::LogLevel::Trace},
                                                      {"debug", ao::log::LogLevel::Debug},
                                                      {"info", ao::log::LogLevel::Info},
                                                      {"warn", ao::log::LogLevel::Warn},
                                                      {"error", ao::log::LogLevel::Error},
                                                      {"critical", ao::log::LogLevel::Critical},
                                                      {"off", ao::log::LogLevel::Off}};

  int verbosity = 0;
  cliApp.add_flag("-v", verbosity, "Verbosity level (-v for debug, -vv for trace)");

  cliApp.add_option("--log-level", logLevel, "Set the logging level")
    ->transform(CLI::CheckedTransformer(logMapping, CLI::ignore_case));

  cliApp.add_flag_callback(
    "--version",
    []()
    {
      std::cout << "Aobus " << ao::kAppVersion << '\n';
      std::exit(0);
    },
    "Show version information");

  try
  {
    cliApp.parse(argc, argv);
  }
  catch (CLI::ParseError const& e)
  {
    return cliApp.exit(e);
  }

  // Handle -v shortcuts if --log-level wasn't explicitly provided (or to override)
  if (cliApp.count("-v") > 0)
  {
    if (verbosity == 1)
    {
      logLevel = ao::log::LogLevel::Debug;
    }
    else if (verbosity >= 2)
    {
      logLevel = ao::log::LogLevel::Trace;
    }
  }

  auto const logDir = std::filesystem::path(Glib::get_user_cache_dir()) / "aobus" / "logs";
  ao::log::Log::init(logLevel, logDir);
  auto const logGuard = gsl_lite::finally([]() { ao::log::Log::shutdown(); });

  APP_LOG_INFO("Aobus {} starting...", ao::kAppVersion);

  Glib::set_application_name("Aobus");

  auto app = Gtk::Application::create("org.aobus.app");

  // Handle Ctrl-C and SIGTERM gracefully via GLib main loop
  auto const signal_handler = [](void* data) -> ::gboolean
  {
    auto* app_ptr = static_cast<Glib::RefPtr<Gtk::Application>*>(data);
    APP_LOG_INFO("Received termination signal, shutting down...");
    (*app_ptr)->quit();
    return FALSE; // Remove source
  };
  ::g_unix_signal_add(SIGINT, signal_handler, &app);
  ::g_unix_signal_add(SIGTERM, signal_handler, &app);

  // Global Theme Refresh: Manual poke for NixOS/Theme changes
  ::g_unix_signal_add(
    SIGUSR1,
    [](void*) -> ::gboolean
    {
      APP_LOG_INFO("Received SIGUSR1, scheduling global theme refresh...");
      ao::gtk::emitThemeRefresh();
      return TRUE;
    },
    nullptr);

  // Auto-detection for NixOS/HM: Monitor GTK config directory for changes
  auto const configPath = std::string(Glib::get_user_config_dir()) + "/gtk-4.0";
  auto const configFile = Gio::File::create_for_path(configPath);
  auto monitor = configFile->monitor_directory();

  // Need to keep the monitor alive, so we attach it to the application or use a static
  static auto monitor_keep_alive = monitor;

  monitor->signal_changed().connect(
    [](
      Glib::RefPtr<Gio::File> const& file, Glib::RefPtr<Gio::File> const& /*other_file*/, Gio::FileMonitor::Event event)
    {
      using Event = Gio::FileMonitor::Event;
      if (event == Event::CHANGED || event == Event::CREATED || event == Event::DELETED ||
          event == Event::CHANGES_DONE_HINT)
      {
        auto const name = file->get_basename();
        if (name == "settings.ini" || name == "gtk.css")
        {
          APP_LOG_INFO(
            "GTK config change detected ({} - event: {}), scheduling refresh...", name, static_cast<int>(event));
          ao::gtk::emitThemeRefresh();
        }
      }
    });

  // DBus Theme Detection (Stylix/Portal support)
  try
  {
    auto bus = Gio::DBus::Connection::get_sync(Gio::DBus::BusType::SESSION);
    if (bus)
    {
      bus->signal_subscribe(
        [](Glib::RefPtr<Gio::DBus::Connection> const&,
           Glib::ustring const&,
           Glib::ustring const&,
           Glib::ustring const&,
           Glib::ustring const&,
           Glib::VariantContainerBase const&)
        {
          APP_LOG_INFO("DBus theme change detected via Portal, scheduling refresh...");
          ao::gtk::emitThemeRefresh();
        },
        "org.freedesktop.portal.Desktop",  // Sender
        "org.freedesktop.portal.Settings", // Interface
        "SettingChanged",                  // Member
        "/org/freedesktop/portal/desktop"  // Path
      );
    }
  }
  catch (Glib::Error const& ex)
  {
    APP_LOG_WARN("Failed to subscribe to DBus theme signals: {}", ex.what());
  }

  // Add about action to application
  auto const aboutAction = Gio::SimpleAction::create("about");
  aboutAction->signal_activate().connect(
    [&app](Glib::VariantBase const& /*variant*/)
    {
      auto dialog = Gtk::AboutDialog{};
      dialog.set_program_name("Aobus");
      dialog.set_version(ao::kAppVersion);
      dialog.set_copyright("Copyright 2024-2026 Aobus Contributors");
      dialog.set_license_type(Gtk::License::LGPL_3_0);

      // Get active window to set as transient parent
      if (auto const windows = app->get_windows(); !windows.empty())
      {
        dialog.set_transient_for(*windows[0]);
      }

      dialog.present();
    });
  app->add_action(aboutAction);

  // Add quit action
  auto const quitAction = Gio::SimpleAction::create("quit");
  quitAction->signal_activate().connect([&app](Glib::VariantBase const& /*variant*/) { app->quit(); });
  app->add_action(quitAction);

  // Store open windows
  auto windows = std::vector<Glib::RefPtr<ao::gtk::MainWindow>>{};

  // Connect to activate signal to create initial window after startup
  app->signal_activate().connect(
    [&app, &windows]()
    {
      auto libraryPath = resolveLibraryPath();

      auto window = createWindow(*app, libraryPath);

      // Wire up the "Open Library" → new window callback
      window->importExportCoordinator().callbacks().onOpenNewLibrary = [&app](std::filesystem::path const& path)
      { createWindow(*app, path); };

      windows.push_back(std::move(window));
    });

  auto remainingArgs = cliApp.remaining_for_passthrough();
  remainingArgs.insert(remainingArgs.begin(), argv[0]);

  std::vector<char*> gtkArgv;
  gtkArgv.reserve(remainingArgs.size());
  for (auto& arg : remainingArgs)
  {
    gtkArgv.push_back(arg.data());
  }
  int gtkArgc = static_cast<int>(gtkArgv.size());

  APP_LOG_INFO("Entering GTK main loop");
  auto const result = app->run(gtkArgc, gtkArgv.data());
  return result;
}
