// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "ao/AppVersion.h"
#include "ao/Exception.h"
#include "ao/utility/Log.h"
#include "app/AppConfig.h"
#include "app/GtkControlExecutor.h"
#include "app/MainWindow.h"
#include "app/StyleManager.h"
#include "portal/ImportExportCoordinator.h"
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ConfigStore.h>
#include <ao/rt/StateTypes.h>

#include <CLI/CLI.hpp>
#include <giomm/simpleaction.h>
#include <glib-unix.h>
#include <glibmm/miscutils.h>
#include <glibmm/refptr.h>
#include <glibmm/variant.h>
#include <gsl-lite/gsl-lite.hpp>
#include <gtkmm/aboutdialog.h>
#include <gtkmm/application.h>

#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <span>
#include <utility>
#include <vector>

using namespace ao;
using namespace ao::gtk;

namespace
{
  struct LibraryPaths final
  {
    std::filesystem::path musicRoot;
    std::filesystem::path databasePath;
  };

  LibraryPaths resolveLibraryPaths(AppConfig const& config)
  {
    auto snapshot = rt::AppPrefsState{};
    config.loadAppPrefs(snapshot);

    if (!snapshot.lastLibraryPath.empty())
    {
      auto musicRoot = std::filesystem::path{snapshot.lastLibraryPath};

      if (std::filesystem::exists(musicRoot))
      {
        return {.musicRoot = musicRoot, .databasePath = musicRoot / ".aobus" / "library"};
      }
    }

    auto const emptyPath = std::filesystem::temp_directory_path() / "aobus-empty";
    std::filesystem::create_directories(emptyPath);
    return {.musicRoot = emptyPath, .databasePath = emptyPath / ".aobus" / "library"};
  }

  Glib::RefPtr<MainWindow> createWindow(Gtk::Application& app, LibraryPaths paths, std::shared_ptr<AppConfig> appConfig)
  {
    auto executor = std::make_unique<GtkControlExecutor>();

    auto const workspaceConfigPath = paths.databasePath / "workspace.yaml";
    auto workspaceConfigStore = std::make_shared<rt::ConfigStore>(workspaceConfigPath);

    auto appRuntime =
      std::make_unique<rt::AppRuntime>(rt::AppRuntimeDependencies{.executor = std::move(executor),
                                                                  .musicRoot = paths.musicRoot,
                                                                  .databasePath = paths.databasePath,
                                                                  .workspaceConfigStore = workspaceConfigStore});

    auto window = Glib::make_refptr_for_instance<MainWindow>(new MainWindow{*appRuntime, appConfig});

    // Store AppRuntime alongside window (lifetime tied to window via pointer)
    window->set_data("app-runtime",
                     new std::unique_ptr<rt::AppRuntime>{std::move(appRuntime)},
                     [](void* data) { delete static_cast<std::unique_ptr<rt::AppRuntime>*>(data); });

    window->initializeSession();

    app.add_window(*window);
    window->present();

    return window;
  }

  struct CliOptions final
  {
    log::LogLevel logLevel = log::LogLevel::Info;
    std::int32_t exitCode = 0;
    bool shouldExit = false;
  };

  CliOptions parseCommandLine(std::span<char*> args)
  {
    auto cliApp = CLI::App{"Aobus Music Library"};
    cliApp.allow_extras(); // Allow GTK specific arguments

    auto options = CliOptions{};

    // Map strings to LogLevel enum for CLI11
    auto const logMapping = std::map<std::string, log::LogLevel>{{"trace", log::LogLevel::Trace},
                                                                 {"debug", log::LogLevel::Debug},
                                                                 {"info", log::LogLevel::Info},
                                                                 {"warn", log::LogLevel::Warn},
                                                                 {"error", log::LogLevel::Error},
                                                                 {"critical", log::LogLevel::Critical},
                                                                 {"off", log::LogLevel::Off}};

    std::int32_t verbosity = 0;
    cliApp.add_flag("-v", verbosity, "Verbosity level (-v for debug, -vv for trace)");

    cliApp.add_option("--log-level", options.logLevel, "Set the logging level")
      ->transform(CLI::CheckedTransformer{logMapping, CLI::ignore_case});

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
      cliApp.parse(static_cast<std::int32_t>(args.size()), args.data());
    }
    catch (CLI::ParseError const& e)
    {
      options.exitCode = cliApp.exit(e);
      options.shouldExit = true;
      return options;
    }

    // Handle -v shortcuts if --log-level wasn't explicitly provided (or to override)
    if (cliApp.count("-v") > 0)
    {
      if (verbosity == 1)
      {
        options.logLevel = log::LogLevel::Debug;
      }
      else if (verbosity >= 2)
      {
        options.logLevel = log::LogLevel::Trace;
      }
    }

    return options;
  }

  void handleOpenNewLibrary(std::filesystem::path const& path,
                            Glib::RefPtr<Gtk::Application> const& app,
                            std::vector<Glib::RefPtr<MainWindow>>& windows,
                            std::shared_ptr<AppConfig> appConfig)
  {
    if (std::filesystem::is_directory(path))
    {
      windows.push_back(
        createWindow(*app, {.musicRoot = path, .databasePath = path / ".aobus" / "library"}, appConfig));
    }
  }
}

int main(int argc, char* argv[])
{
  try
  {
    auto const options = parseCommandLine({argv, static_cast<std::size_t>(argc)});

    if (options.shouldExit)
    {
      return options.exitCode;
    }

    auto const logDir = std::filesystem::path{Glib::get_user_cache_dir()} / "aobus" / "logs";
    log::Log::init(options.logLevel, logDir);
    auto const logGuard = gsl_lite::finally([] { log::Log::shutdown(); });

    APP_LOG_INFO("Aobus {} starting...", kAppVersion);

    Glib::set_application_name("Aobus");

    auto app = Gtk::Application::create("org.aobus.app");

    // Handle Ctrl-C and SIGTERM gracefully via GLib main loop
    auto const signalHandler = [](void* data) -> ::gboolean
    {
      auto* appPtr = static_cast<Glib::RefPtr<Gtk::Application>*>(data);
      APP_LOG_INFO("Received termination signal, shutting down...");
      (*appPtr)->quit();
      return FALSE; // Remove source
    };
    ::g_unix_signal_add(SIGINT, signalHandler, &app);
    ::g_unix_signal_add(SIGTERM, signalHandler, &app);

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

        auto const globalConfigPath = std::filesystem::path{Glib::get_user_config_dir()} / "aobus" / "config.yaml";
        auto appConfig = std::make_shared<AppConfig>(globalConfigPath);

        auto paths = resolveLibraryPaths(*appConfig);

        auto window = createWindow(*app, std::move(paths), appConfig);

        // Wire up the "Open Library" → new window callback
        window->importExportCoordinator().callbacks().onOpenNewLibrary =
          [&app, &windows, appConfig](std::filesystem::path const& path)
        { handleOpenNewLibrary(path, app, windows, appConfig); };

        windows.push_back(std::move(window));
      });

    // Consuming CLI arguments for GTK passthrough
    auto cliAppInternal = CLI::App{};
    cliAppInternal.allow_extras();

    try
    {
      cliAppInternal.parse(argc, argv);
    }
    catch (CLI::ParseError const& e)
    {
      APP_LOG_DEBUG("Internal CLI parse for GTK passthrough: {}", e.what());
      // Note: Error reporting is primarily handled by the first pass in parseCommandLine
    }

    auto remainingArgs = cliAppInternal.remaining_for_passthrough();
    remainingArgs.insert(remainingArgs.begin(), argv[0]);
    auto gtkArgv = std::vector<char*>{};
    gtkArgv.reserve(remainingArgs.size());

    for (auto& arg : remainingArgs)
    {
      gtkArgv.push_back(arg.data());
    }

    std::int32_t const gtkArgc = static_cast<std::int32_t>(gtkArgv.size());

    APP_LOG_INFO("Entering GTK main loop");
    return app->run(gtkArgc, gtkArgv.data());
  }
  catch (ao::Exception const& e)
  {
    APP_LOG_CRITICAL("Internal error: {} (at {}:{})", e.what(), e.file(), e.line());
    std::cerr << "Internal error: " << e.what() << "\n(at " << e.file() << ":" << e.line() << ")\n"
              << "Please report this bug.\n";
    return 1;
  }
  catch (std::exception const& ex)
  {
    APP_LOG_CRITICAL("Unhandled exception: {}", ex.what());
    std::cerr << "Unhandled exception: " << ex.what() << '\n';
    return 1;
  }
  catch (...)
  {
    std::cerr << "Unknown unhandled exception" << '\n';
    return 1;
  }
}
