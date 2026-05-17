// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "app/GtkControlExecutor.h"
#include "app/MainWindow.h"
#include "app/StyleManager.h"
#include "library_io/ImportExportCoordinator.h"
#include <ao/utility/Log.h>
#include <giomm/simpleaction.h>
#include <runtime/AppRuntime.h>

#include <ao/AppVersion.h>

#include <glibmm/refptr.h>
#include <glibmm/variant.h>
#include <gsl-lite/gsl-lite.hpp>
#include <gtkmm/aboutdialog.h>
#include <gtkmm/application.h>

#include <glib-unix.h>

#include <CLI/CLI.hpp>

#include <runtime/ConfigStore.h>
#include <runtime/StateTypes.h>

#include <glibmm/miscutils.h>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <utility>
#include <vector>

using namespace ao;
using namespace ao::gtk;

namespace
{
  std::filesystem::path resolveLibraryPath()
  {
    {
      auto const configPath = std::filesystem::path{Glib::get_user_config_dir()} / "aobus" / "config.yaml";
      auto store = rt::ConfigStore{configPath};
      auto snapshot = rt::SessionSnapshot{};

      if (auto result = store.load("runtime", snapshot); !result)
      {
        APP_LOG_WARN("Failed to load runtime config: {}", result.error().message);
      }

      auto const& path = snapshot.lastLibraryPath;

      if (!path.empty())
      {
        return std::filesystem::path{path};
      }
    }

    auto const emptyPath = std::filesystem::temp_directory_path() / "aobus-empty";
    std::filesystem::create_directories(emptyPath);
    return emptyPath;
  }

  Glib::RefPtr<MainWindow> createWindow(Gtk::Application& app, std::filesystem::path libraryPath)
  {
    auto executor = std::make_shared<GtkControlExecutor>();
    auto const configPath = std::filesystem::path{Glib::get_user_config_dir()} / "aobus" / "config.yaml";
    auto configStore = std::make_shared<rt::ConfigStore>(configPath);

    auto appRuntime = std::make_unique<rt::AppRuntime>(rt::AppRuntimeDependencies{
      .executor = std::move(executor), .libraryRoot = std::move(libraryPath), .configStore = configStore});

    // NOLINTBEGIN(cppcoreguidelines-owning-memory)
    auto window = Glib::make_refptr_for_instance<MainWindow>(new MainWindow(*appRuntime, configStore));

    // Store AppRuntime alongside window (lifetime tied to window via pointer)
    window->set_data("app-runtime",
                     new std::unique_ptr<rt::AppRuntime>(std::move(appRuntime)),
                     [](void* data) { delete static_cast<std::unique_ptr<rt::AppRuntime>*>(data); });
    // NOLINTEND(cppcoreguidelines-owning-memory)

    window->initializeSession();

    app.add_window(*window);
    window->present();

    return window;
  }
}

// NOLINTBEGIN(bugprone-exception-escape,readability-function-cognitive-complexity)
int main(int argc, char* argv[])
{
  CLI::App cliApp{"Aobus Music Library"};
  cliApp.allow_extras(); // Allow GTK specific arguments

  auto logLevel = log::LogLevel::Info;

  // Map strings to LogLevel enum for CLI11
  auto const logMapping = std::map<std::string, log::LogLevel>{{"trace", log::LogLevel::Trace},
                                                               {"debug", log::LogLevel::Debug},
                                                               {"info", log::LogLevel::Info},
                                                               {"warn", log::LogLevel::Warn},
                                                               {"error", log::LogLevel::Error},
                                                               {"critical", log::LogLevel::Critical},
                                                               {"off", log::LogLevel::Off}};

  int verbosity = 0;
  cliApp.add_flag("-v", verbosity, "Verbosity level (-v for debug, -vv for trace)");

  cliApp.add_option("--log-level", logLevel, "Set the logging level")
    ->transform(CLI::CheckedTransformer(logMapping, CLI::ignore_case));

  cliApp.add_flag_callback(
    "--version",
    []
    {
      std::cout << "Aobus " << kAppVersion << '\n';
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
      logLevel = log::LogLevel::Debug;
    }
    else if (verbosity >= 2)
    {
      logLevel = log::LogLevel::Trace;
    }
  }

  auto const logDir = std::filesystem::path(Glib::get_user_cache_dir()) / "aobus" / "logs";
  log::Log::init(logLevel, logDir);
  auto const logGuard = gsl_lite::finally([] { log::Log::shutdown(); });

  APP_LOG_INFO("Aobus {} starting...", kAppVersion);

  Glib::set_application_name("Aobus");

  auto app = Gtk::Application::create("org.aobus.app");

  // Handle Ctrl-C and SIGTERM gracefully via GLib main loop
  // NOLINTBEGIN(misc-include-cleaner)
  auto const signal_handler = [](void* data) -> ::gboolean
  {
    auto* app_ptr = static_cast<Glib::RefPtr<Gtk::Application>*>(data);
    APP_LOG_INFO("Received termination signal, shutting down...");
    (*app_ptr)->quit();
    return FALSE; // Remove source
  };
  ::g_unix_signal_add(SIGINT, signal_handler, &app);
  ::g_unix_signal_add(SIGTERM, signal_handler, &app);
  // NOLINTEND(misc-include-cleaner)

  // Add about action to application
  auto const aboutAction = Gio::SimpleAction::create("about");
  aboutAction->signal_activate().connect(
    [&app](Glib::VariantBase const& /*variant*/)
    {
      auto dialog = Gtk::AboutDialog{};
      dialog.set_program_name("Aobus");
      dialog.set_version(kAppVersion);
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
  auto windows = std::vector<Glib::RefPtr<MainWindow>>{};

  // Connect to activate signal to create initial window after startup
  app->signal_activate().connect(
    [&app, &windows]
    {
      // Initialize the unified style system before any window is created.
      // Idempotent — safe to call across multiple activate signals.
      StyleManager::instance().initialize();

      auto libraryPath = resolveLibraryPath();

      auto window = createWindow(*app, libraryPath);

      // Wire up the "Open Library" → new window callback
      window->importExportCoordinator().callbacks().onOpenNewLibrary = [&app](std::filesystem::path const& path)
      { createWindow(*app, path); };

      windows.push_back(std::move(window));
    });

  auto remainingArgs = cliApp.remaining_for_passthrough();
  remainingArgs.insert(remainingArgs.begin(), argv[0]);

  auto gtkArgv = std::vector<char*>{};
  gtkArgv.reserve(remainingArgs.size());

  for (auto& arg : remainingArgs)
  {
    gtkArgv.push_back(arg.data());
  }

  int const gtkArgc = static_cast<int>(gtkArgv.size());

  APP_LOG_INFO("Entering GTK main loop");
  auto const result = app->run(gtkArgc, gtkArgv.data());
  return result;
}
// NOLINTEND(bugprone-exception-escape,readability-function-cognitive-complexity)
