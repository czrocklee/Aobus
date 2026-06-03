// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "app/AppConfig.h"
#include "app/GtkControlExecutor.h"
#include "app/GtkStyleRuntime.h"
#include "app/MainWindow.h"
#include "portal/ImportExportCoordinator.h"
#include <ao/AppVersion.h>
#include <ao/Exception.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ConfigStore.h>
#include <ao/rt/StateTypes.h>
#include <ao/utility/Log.h>

#include <CLI/CLI.hpp>
#include <giomm/simpleaction.h>
#include <glib-unix.h>
#include <glibmm/miscutils.h>
#include <glibmm/refptr.h>
#include <glibmm/variant.h>
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

  Glib::RefPtr<MainWindow> createWindow(Gtk::Application& app,
                                        LibraryPaths paths,
                                        std::shared_ptr<AppConfig> appConfigPtr)
  {
    auto executorPtr = std::make_unique<GtkControlExecutor>();

    auto const workspaceConfigPath = paths.databasePath / "workspace.yaml";
    auto workspaceConfigStorePtr = std::make_unique<rt::ConfigStore>(workspaceConfigPath);

    auto appRuntimePtr = std::make_unique<rt::AppRuntime>(
      rt::AppRuntimeDependencies{.executorPtr = std::move(executorPtr),
                                 .musicRoot = paths.musicRoot,
                                 .databasePath = paths.databasePath,
                                 .workspaceConfigStorePtr = std::move(workspaceConfigStorePtr)});

    auto windowPtr = Glib::make_refptr_for_instance<MainWindow>(new MainWindow{*appRuntimePtr, appConfigPtr});

    // Store AppRuntime alongside window (lifetime tied to window via pointer)
    windowPtr->set_data("app-runtime",
                        new std::unique_ptr<rt::AppRuntime>{std::move(appRuntimePtr)},
                        [](void* data) { delete static_cast<std::unique_ptr<rt::AppRuntime>*>(data); });

    windowPtr->initializeSession();

    app.add_window(*windowPtr);
    windowPtr->present();

    return windowPtr;
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
                            std::shared_ptr<AppConfig> appConfigPtr)
  {
    if (std::filesystem::is_directory(path))
    {
      windows.push_back(
        createWindow(*app, {.musicRoot = path, .databasePath = path / ".aobus" / "library"}, appConfigPtr));
    }
  }

  void releaseWindows(Gtk::Application& app, std::vector<Glib::RefPtr<MainWindow>>& windows)
  {
    for (auto& window : windows)
    {
      if (window)
      {
        app.remove_window(*window);
        window.reset();
      }
    }

    windows.clear();
  }

  void setupUnixSignalHandlers(Glib::RefPtr<Gtk::Application>& app)
  {
    auto const handler = [](void* data) -> ::gboolean
    {
      auto* appCtx = static_cast<Glib::RefPtr<Gtk::Application>*>(data);
      APP_LOG_INFO("Received termination signal, shutting down...");
      (*appCtx)->quit();
      return FALSE;
    };
    ::g_unix_signal_add(SIGINT, handler, &app);
    ::g_unix_signal_add(SIGTERM, handler, &app);
  }

  void addAppActions(Glib::RefPtr<Gtk::Application>& app)
  {
    auto const aboutActionPtr = Gio::SimpleAction::create("about");
    aboutActionPtr->signal_activate().connect(
      [&app](Glib::VariantBase const& /*variant*/)
      {
        auto dialog = Gtk::AboutDialog{};
        dialog.set_program_name("Aobus");
        dialog.set_version(kAppVersion);
        dialog.set_copyright("Copyright 2024-2026 Aobus Contributors");
        dialog.set_license_type(Gtk::License::LGPL_3_0);

        if (auto const windows = app->get_windows(); !windows.empty())
        {
          dialog.set_transient_for(*windows[0]);
        }

        dialog.present();
      });
    app->add_action(aboutActionPtr);

    auto const quitActionPtr = Gio::SimpleAction::create("quit");
    quitActionPtr->signal_activate().connect([&app](Glib::VariantBase const& /*variant*/) { app->quit(); });
    app->add_action(quitActionPtr);

    // Global layout action shortcuts (exported to the window ActionMap, so they use the 'win.' prefix)
    app->set_accels_for_action("win.playback.playPause", {"<Primary>p", "space", "AudioPlay", "AudioPause"});
    app->set_accels_for_action("win.playback.stop", {"AudioStop"});
    app->set_accels_for_action("win.playback.next", {"<Primary>Right", "AudioNext"});
    app->set_accels_for_action("win.playback.previous", {"<Primary>Left", "AudioPrev"});
    app->set_accels_for_action("win.playback.toggleShuffle", {"<Primary>u"});
    app->set_accels_for_action("win.playback.cycleRepeat", {"<Primary>r"});
    app->set_accels_for_action("win.workspace.revealCurrentTrack", {"<Primary>l"});
  }

  void onAppActivate(Glib::RefPtr<Gtk::Application>& app, std::vector<Glib::RefPtr<MainWindow>>& windows)
  {
    GtkStyleRuntime::instance().initialize();

    auto const globalConfigPath = std::filesystem::path{Glib::get_user_config_dir()} / "aobus" / "config.yaml";
    auto appConfigPtr = std::make_shared<AppConfig>(globalConfigPath);

    auto paths = resolveLibraryPaths(*appConfigPtr);

    auto windowPtr = createWindow(*app, std::move(paths), appConfigPtr);

    windowPtr->importExportCoordinator().callbacks().onOpenNewLibrary =
      [&app, &windows, appConfigPtr](std::filesystem::path const& path)
    { handleOpenNewLibrary(path, app, windows, appConfigPtr); };

    windows.push_back(std::move(windowPtr));
  }

  // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
  std::vector<char*> buildGtkArgv(std::int32_t argc, char* argv[])
  {
    auto cliApp = CLI::App{};
    cliApp.allow_extras();

    try
    {
      cliApp.parse(argc, argv);
    }
    catch (CLI::ParseError const& e)
    {
      APP_LOG_DEBUG("Internal CLI parse for GTK passthrough: {}", e.what());
    }

    auto remaining = cliApp.remaining_for_passthrough();
    remaining.insert(remaining.begin(), argv[0]);
    auto gtkArgv = std::vector<char*>{};
    gtkArgv.reserve(remaining.size());

    for (auto& arg : remaining)
    {
      gtkArgv.push_back(arg.data());
    }

    return gtkArgv;
  }
}

int main(int argc, char* argv[])
{
  auto exitCode = 0;

  try
  {
    auto const options = parseCommandLine({argv, static_cast<std::size_t>(argc)});

    if (options.shouldExit)
    {
      return options.exitCode;
    }

    auto const logDir = std::filesystem::path{Glib::get_user_cache_dir()} / "aobus" / "logs";
    log::Log::init(options.logLevel, logDir);

    APP_LOG_INFO("Aobus {} starting...", kAppVersion);

    Glib::set_application_name("Aobus");

    auto appPtr = Gtk::Application::create("org.aobus.app");

    setupUnixSignalHandlers(appPtr);

    addAppActions(appPtr);

    auto windows = std::vector<Glib::RefPtr<MainWindow>>{};

    appPtr->signal_activate().connect([&appPtr, &windows] { onAppActivate(appPtr, windows); });

    auto gtkArgv = buildGtkArgv(argc, argv);
    std::int32_t const gtkArgc = static_cast<std::int32_t>(gtkArgv.size());

    APP_LOG_INFO("Entering GTK main loop");
    exitCode = appPtr->run(gtkArgc, gtkArgv.data());

    releaseWindows(*appPtr, windows);
  }
  catch (ao::Exception const& e)
  {
    APP_LOG_CRITICAL("Internal error: {} (at {}:{})", e.what(), e.file(), e.line());
    std::cerr << "Internal error: " << e.what() << "\n(at " << e.file() << ":" << e.line() << ")\n"
              << "Please report this bug.\n";
    exitCode = 1;
  }
  catch (std::exception const& ex)
  {
    APP_LOG_CRITICAL("Unhandled exception: {}", ex.what());
    std::cerr << "Unhandled exception: " << ex.what() << '\n';
    exitCode = 1;
  }
  catch (...)
  {
    std::cerr << "Unknown unhandled exception" << '\n';
    exitCode = 1;
  }

  log::Log::shutdown();
  return exitCode;
}
