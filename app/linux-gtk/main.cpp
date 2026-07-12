// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/AppConfigStore.h"
#include "app/AppDialog.h"
#include "app/GtkMainContextExecutor.h"
#include "app/GtkStyleRuntime.h"
#include "app/KeymapApplicator.h"
#include "app/MainWindow.h"
#include "app/ShellLayoutComponentStateStore.h"
#include "app/ShellLayoutStore.h"
#include "platform/AudioBackendBootstrap.h"
#include "portal/ImportExportCoordinator.h"
#include "portal/ImportExportCoordinatorPolicy.h"
#include "portal/LibraryImportExportWorkflow.h"
#include "preferences/PreferencesWindow.h"
#include <ao/AppVersion.h>
#include <ao/Exception.h>
#include <ao/rt/AppPrefsState.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ConfigStore.h>
#include <ao/rt/Log.h>
#include <ao/uimodel/input/KeymapModel.h>
#include <ao/uimodel/preferences/PreferencesEditorModel.h>

#include <CLI/CLI.hpp>
#include <giomm/simpleaction.h>
#include <glib-unix.h>
#include <glibmm/exceptionhandler.h>
#include <glibmm/main.h>
#include <glibmm/miscutils.h>
#include <glibmm/refptr.h>
#include <glibmm/variant.h>
#include <gtkmm/aboutdialog.h>
#include <gtkmm/application.h>
#include <gtkmm/dialog.h>
#include <gtkmm/window.h>

#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <map>
#include <memory>
#include <print>
#include <span>
#include <string>
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
    bool scanAfterOpen = false;
  };

  LibraryPaths resolveLibraryPaths(AppConfigStore const& configStore)
  {
    auto snapshot = rt::AppSessionState{};
    configStore.loadAppSession(snapshot);

    if (!snapshot.lastLibraryPath.empty())
    {
      auto musicRoot = std::filesystem::path{snapshot.lastLibraryPath};

      if (std::filesystem::exists(musicRoot))
      {
        return {.musicRoot = musicRoot,
                .databasePath = portal::defaultLibraryDatabasePath(musicRoot),
                .scanAfterOpen = portal::shouldScanAfterOpen(musicRoot)};
      }
    }

    auto const emptyPath = std::filesystem::temp_directory_path() / "aobus-empty";
    std::filesystem::create_directories(emptyPath);
    return {.musicRoot = emptyPath, .databasePath = portal::defaultLibraryDatabasePath(emptyPath)};
  }

  Glib::RefPtr<MainWindow> createWindow(Gtk::Application& app,
                                        LibraryPaths paths,
                                        std::shared_ptr<AppConfigStore> appConfigStorePtr,
                                        std::shared_ptr<ShellLayoutStore> shellLayoutStorePtr,
                                        std::shared_ptr<ShellLayoutComponentStateStore> componentStateStorePtr)
  {
    auto executorPtr = std::make_unique<GtkMainContextExecutor>();

    auto const workspaceConfigPath = paths.databasePath / "workspace.yaml";
    auto workspaceConfigStorePtr = std::make_unique<rt::ConfigStore>(workspaceConfigPath);

    auto appRuntimePtr = std::make_unique<rt::AppRuntime>(
      rt::AppRuntimeDependencies{.executorPtr = std::move(executorPtr),
                                 .musicRoot = paths.musicRoot,
                                 .databasePath = paths.databasePath,
                                 .workspaceConfigStorePtr = std::move(workspaceConfigStorePtr),
                                 .playbackSessionConfigStore = &appConfigStorePtr->playbackSessionStore()});

    registerPlatformAudioBackends(*appRuntimePtr);

    auto windowPtr = Glib::make_refptr_for_instance<MainWindow>(
      new MainWindow{*appRuntimePtr, appConfigStorePtr, shellLayoutStorePtr, componentStateStorePtr});

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
    rt::LogLevel logLevel = rt::LogLevel::Info;
    std::int32_t exitCode = 0;
    bool shouldExit = false;
  };

  CliOptions parseCommandLine(std::span<char*> args)
  {
    auto cliApp = CLI::App{"Aobus Music Library"};
    cliApp.allow_extras(); // Allow GTK specific arguments

    auto options = CliOptions{};

    // Map strings to LogLevel enum for CLI11
    auto const logMapping = std::map<std::string, rt::LogLevel>{{"trace", rt::LogLevel::Trace},
                                                                {"debug", rt::LogLevel::Debug},
                                                                {"info", rt::LogLevel::Info},
                                                                {"warn", rt::LogLevel::Warn},
                                                                {"error", rt::LogLevel::Error},
                                                                {"critical", rt::LogLevel::Critical},
                                                                {"off", rt::LogLevel::Off}};

    std::int32_t verbosity = 0;
    cliApp.add_flag("-v", verbosity, "Verbosity level (-v for debug, -vv for trace)");

    cliApp.add_option("--log-level", options.logLevel, "Set the logging level")
      ->transform(CLI::CheckedTransformer{logMapping, CLI::ignore_case});

    cliApp.add_flag_callback(
      "--version",
      []
      {
        std::println("Aobus {}", kAppVersion);
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
        options.logLevel = rt::LogLevel::Debug;
      }
      else if (verbosity >= 2)
      {
        options.logLevel = rt::LogLevel::Trace;
      }
    }

    return options;
  }

  void configureOpenLibraryCallback(Glib::RefPtr<MainWindow> const& windowPtr,
                                    Glib::RefPtr<Gtk::Application> const& appPtr,
                                    Glib::RefPtr<MainWindow>& mainWindowPtr,
                                    std::shared_ptr<AppConfigStore> appConfigStorePtr,
                                    std::shared_ptr<ShellLayoutStore> shellLayoutStorePtr,
                                    std::shared_ptr<ShellLayoutComponentStateStore> componentStateStorePtr);

  void handleOpenNewLibrary(std::filesystem::path const& path,
                            Glib::RefPtr<Gtk::Application> const& appPtr,
                            Glib::RefPtr<MainWindow>& mainWindowPtr,
                            std::shared_ptr<AppConfigStore> appConfigStorePtr,
                            std::shared_ptr<ShellLayoutStore> shellLayoutStorePtr,
                            std::shared_ptr<ShellLayoutComponentStateStore> componentStateStorePtr,
                            bool const scanAfterOpen)
  {
    if (!std::filesystem::is_directory(path))
    {
      return;
    }

    auto const requestedPath = std::filesystem::absolute(path).lexically_normal();

    if (mainWindowPtr && mainWindowPtr->musicRoot() == requestedPath)
    {
      if (scanAfterOpen)
      {
        mainWindowPtr->importExportCoordinator().scanLibrary(portal::ScanRequestMode::FastBootstrap);
      }

      mainWindowPtr->present();
      return;
    }

    if (mainWindowPtr)
    {
      if (auto const prepared = mainWindowPtr->prepareForLibrarySwitch(); !prepared)
      {
        APP_LOG_ERROR("Failed to prepare active library for replacement: {}", prepared.error().message);
        return;
      }

      appPtr->remove_window(*mainWindowPtr);
      mainWindowPtr.reset();
    }

    auto appSession = rt::AppSessionState{};
    appConfigStorePtr->loadAppSession(appSession);
    appSession.lastLibraryPath = requestedPath.string();
    appConfigStorePtr->saveAppSession(appSession);

    mainWindowPtr = createWindow(*appPtr,
                                 {.musicRoot = requestedPath,
                                  .databasePath = portal::defaultLibraryDatabasePath(requestedPath),
                                  .scanAfterOpen = scanAfterOpen},
                                 appConfigStorePtr,
                                 shellLayoutStorePtr,
                                 componentStateStorePtr);
    configureOpenLibraryCallback(
      mainWindowPtr, appPtr, mainWindowPtr, appConfigStorePtr, shellLayoutStorePtr, componentStateStorePtr);

    if (scanAfterOpen)
    {
      mainWindowPtr->importExportCoordinator().scanLibrary(portal::ScanRequestMode::FastBootstrap);
    }
  }

  void configureOpenLibraryCallback(Glib::RefPtr<MainWindow> const& windowPtr,
                                    Glib::RefPtr<Gtk::Application> const& appPtr,
                                    Glib::RefPtr<MainWindow>& mainWindowPtr,
                                    std::shared_ptr<AppConfigStore> appConfigStorePtr,
                                    std::shared_ptr<ShellLayoutStore> shellLayoutStorePtr,
                                    std::shared_ptr<ShellLayoutComponentStateStore> componentStateStorePtr)
  {
    windowPtr->importExportCoordinator().callbacks().onOpenNewLibrary =
      [appPtr, &mainWindowPtr, appConfigStorePtr, shellLayoutStorePtr, componentStateStorePtr](
        std::filesystem::path const& path, bool const scanAfterOpen)
    {
      Glib::signal_idle().connect_once(
        [path, scanAfterOpen, appPtr, &mainWindowPtr, appConfigStorePtr, shellLayoutStorePtr, componentStateStorePtr]
        {
          handleOpenNewLibrary(
            path, appPtr, mainWindowPtr, appConfigStorePtr, shellLayoutStorePtr, componentStateStorePtr, scanAfterOpen);
        });
    };
  }

  void releaseMainWindow(Gtk::Application& app, Glib::RefPtr<MainWindow>& mainWindowPtr)
  {
    if (!mainWindowPtr)
    {
      return;
    }

    try
    {
      mainWindowPtr->saveSession();
    }
    catch (std::exception const& e)
    {
      APP_LOG_ERROR("Failed to save runtime during shutdown: {}", e.what());
    }
    catch (...)
    {
      APP_LOG_ERROR("Failed to save runtime during shutdown: unknown exception");
    }

    app.remove_window(*mainWindowPtr);
    mainWindowPtr.reset();
  }

  void installUnixSignalHandlers(Glib::RefPtr<Gtk::Application>& appPtr)
  {
    auto const handler = [](void* data) -> ::gboolean
    {
      auto* appCtx = static_cast<Glib::RefPtr<Gtk::Application>*>(data);
      APP_LOG_INFO("Received termination signal, shutting down...");
      (*appCtx)->quit();
      return FALSE;
    };
    ::g_unix_signal_add(SIGINT, handler, &appPtr);
    ::g_unix_signal_add(SIGTERM, handler, &appPtr);
  }

  MainWindow* activeMainWindow(Glib::RefPtr<Gtk::Application> const& appPtr)
  {
    if (auto* const activeWindow = appPtr->get_active_window(); activeWindow != nullptr)
    {
      if (auto* const mainWindow = dynamic_cast<MainWindow*>(activeWindow); mainWindow != nullptr)
      {
        return mainWindow;
      }

      if (auto* const transient = activeWindow->get_transient_for(); transient != nullptr)
      {
        if (auto* const mainWindow = dynamic_cast<MainWindow*>(transient); mainWindow != nullptr)
        {
          return mainWindow;
        }
      }
    }

    for (auto* const window : appPtr->get_windows())
    {
      if (auto* const mainWindow = dynamic_cast<MainWindow*>(window); mainWindow != nullptr)
      {
        return mainWindow;
      }
    }

    return nullptr;
  }

  void applyThemeToMainWindows(Glib::RefPtr<Gtk::Application> const& appPtr, rt::ThemePresetId const theme)
  {
    for (auto* const window : appPtr->get_windows())
    {
      if (auto* const mainWindow = dynamic_cast<MainWindow*>(window); mainWindow != nullptr)
      {
        mainWindow->applyTheme(theme);
      }
    }
  }

  void presentPreferences(Glib::RefPtr<Gtk::Application> const& appPtr,
                          std::unique_ptr<PreferencesWindow>& preferencesWindowPtr,
                          std::shared_ptr<AppConfigStore> const& appConfigStorePtr)
  {
    auto* const targetWindow = activeMainWindow(appPtr);

    if (targetWindow == nullptr || !appConfigStorePtr)
    {
      return;
    }

    if (!preferencesWindowPtr)
    {
      preferencesWindowPtr = std::make_unique<PreferencesWindow>(PreferencesWindow::Callbacks{
        .onEditLayout =
          [appPtr]
        {
          if (auto* const window = activeMainWindow(appPtr); window != nullptr)
          {
            window->openLayoutEditor();
          }
        },
        .onResetRuntimeLayoutState =
          [appPtr]
        {
          if (auto* const window = activeMainWindow(appPtr); window != nullptr)
          {
            window->resetRuntimeLayoutState();
          }
        },
        .onSaveCurrentPanelSizesAsLayoutDefaults =
          [appPtr]
        {
          if (auto* const window = activeMainWindow(appPtr); window != nullptr)
          {
            window->saveCurrentPanelSizesAsLayoutDefaults();
          }
        },
        .onPersistPreferences =
          [appConfigStorePtr](rt::AppPrefsState const& prefs, uimodel::PreferencesChange const change)
        {
          auto current = rt::AppPrefsState{};
          appConfigStorePtr->loadAppPrefs(current);
          appConfigStorePtr->saveAppPrefs(uimodel::mergePreferenceChange(std::move(current), prefs, change));
        },
        .onApplyTheme = [appPtr](rt::ThemePresetId const theme) { applyThemeToMainWindows(appPtr, theme); },
      });
    }

    if (!preferencesWindowPtr->get_application())
    {
      appPtr->add_window(*preferencesWindowPtr);
    }

    preferencesWindowPtr->set_transient_for(*targetWindow);
    auto prefs = rt::AppPrefsState{};
    appConfigStorePtr->loadAppPrefs(prefs);
    preferencesWindowPtr->refreshPreferences(prefs, &targetWindow->playbackService(), targetWindow);
    preferencesWindowPtr->refreshKeyboardPage(targetWindow->layoutActionCatalog(),
                                              appConfigStorePtr->loadKeymap(uimodel::defaultKeymap()),
                                              [appPtr](uimodel::KeymapModel const& keymap)
                                              {
                                                if (auto* const window = activeMainWindow(appPtr); window != nullptr)
                                                {
                                                  window->applyKeymap(keymap);
                                                }
                                              });
    preferencesWindowPtr->present();
  }

  void addAppActions(Glib::RefPtr<Gtk::Application>& appPtr,
                     std::unique_ptr<PreferencesWindow>& preferencesWindowPtr,
                     std::shared_ptr<AppConfigStore> const& appConfigStorePtr)
  {
    auto const aboutActionPtr = Gio::SimpleAction::create("about");
    aboutActionPtr->signal_activate().connect(
      [&appPtr](Glib::VariantBase const& /*variant*/)
      {
        auto dialog = Gtk::AboutDialog{};
        dialog.set_program_name("Aobus");
        dialog.set_version(kAppVersion);
        dialog.set_copyright("Copyright 2024-2026 Aobus Contributors");
        dialog.set_license_type(Gtk::License::LGPL_3_0);

        if (auto const windows = appPtr->get_windows(); !windows.empty())
        {
          dialog.set_transient_for(*windows[0]);
        }

        dialog.present();
      });
    appPtr->add_action(aboutActionPtr);

    auto const quitActionPtr = Gio::SimpleAction::create("quit");
    quitActionPtr->signal_activate().connect([&appPtr](Glib::VariantBase const& /*variant*/) { appPtr->quit(); });
    appPtr->add_action(quitActionPtr);

    auto const preferencesActionPtr = Gio::SimpleAction::create("preferences");
    preferencesActionPtr->signal_activate().connect(
      [&appPtr, &preferencesWindowPtr, appConfigStorePtr](Glib::VariantBase const& /*variant*/)
      { presentPreferences(appPtr, preferencesWindowPtr, appConfigStorePtr); });
    appPtr->add_action(preferencesActionPtr);
    appPtr->set_accels_for_action("app.preferences", {"<Control>comma"});
  }

  std::filesystem::path layoutStateDir()
  {
    auto const* const xdgStateHome = std::getenv("XDG_STATE_HOME");

    if (xdgStateHome != nullptr && xdgStateHome[0] != '\0')
    {
      return std::filesystem::path{xdgStateHome} / "aobus" / "layout-state";
    }

    return std::filesystem::path{Glib::get_user_data_dir()}.parent_path() / "state" / "aobus" / "layout-state";
  }

  void handleAppActivate(Glib::RefPtr<Gtk::Application>& appPtr,
                         Glib::RefPtr<MainWindow>& mainWindowPtr,
                         std::shared_ptr<AppConfigStore> const& appConfigStorePtr,
                         std::shared_ptr<ShellLayoutStore> const& shellLayoutStorePtr,
                         std::shared_ptr<ShellLayoutComponentStateStore> const& componentStateStorePtr)
  {
    GtkStyleRuntime::instance().initialize();

    applyKeymapAccelerators(*appPtr, appConfigStorePtr->loadKeymap(uimodel::defaultKeymap()));

    if (mainWindowPtr)
    {
      mainWindowPtr->present();
      return;
    }

    auto paths = resolveLibraryPaths(*appConfigStorePtr);

    auto const scanAfterOpen = paths.scanAfterOpen;
    mainWindowPtr =
      createWindow(*appPtr, std::move(paths), appConfigStorePtr, shellLayoutStorePtr, componentStateStorePtr);
    configureOpenLibraryCallback(
      mainWindowPtr, appPtr, mainWindowPtr, appConfigStorePtr, shellLayoutStorePtr, componentStateStorePtr);

    if (scanAfterOpen)
    {
      mainWindowPtr->importExportCoordinator().scanLibrary(portal::ScanRequestMode::FastBootstrap);
    }
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

    for (auto& argument : remaining)
    {
      gtkArgv.push_back(argument.data());
    }

    return gtkArgv;
  }

  void handleSignalException(Glib::RefPtr<Gtk::Application> const& appPtr)
  {
    auto detail = std::string{};

    try
    {
      throw;
    }
    catch (ao::Exception const& e)
    {
      APP_LOG_ERROR("Unhandled exception escaped a signal handler: {} (at {}:{})", e.what(), e.file(), e.line());
      detail = e.what();
    }
    catch (std::exception const& e)
    {
      APP_LOG_ERROR("Unhandled exception escaped a signal handler: {}", e.what());
      detail = e.what();
    }
    catch (...)
    {
      APP_LOG_ERROR("Unhandled non-standard exception escaped a signal handler");
      detail = "Unknown error";
    }

    if (auto* const window = appPtr->get_active_window(); window != nullptr)
    {
      AppDialog::presentMessage(
        *window,
        "The operation could not be completed.",
        detail,
        {AppDialogAction{.label = "OK", .responseId = Gtk::ResponseType::OK, .role = AppDialogActionRole::Primary}},
        Gtk::ResponseType::OK);
    }
  }

  std::int32_t runApp(std::span<char*> args)
  {
    auto const options = parseCommandLine(args);

    if (options.shouldExit)
    {
      return options.exitCode;
    }

    auto const logDir = std::filesystem::path{Glib::get_user_cache_dir()} / "aobus" / "logs";
    rt::Log::initialize(options.logLevel, logDir);

    APP_LOG_INFO("Aobus {} starting...", kAppVersion);

    Glib::set_application_name("Aobus");

    auto appPtr = Gtk::Application::create("org.aobus.app");

    // Top-level boundary for exceptions that escape a GTK signal/action handler.
    // Such exceptions must not unwind through glib's C frames (UB), so glibmm
    // catches them at the slot boundary and routes here. By this point the
    // throwing operation's RAII has already run (e.g. an uncommitted LMDB write
    // transaction aborts on destruction), so the data store stays consistent;
    // we log, surface a generic notice, and let the app keep running rather than
    // terminate on a transient failure.
    Glib::add_exception_handler([appPtr] { handleSignalException(appPtr); });

    installUnixSignalHandlers(appPtr);

    auto mainWindowPtr = Glib::RefPtr<MainWindow>{};
    auto preferencesWindowPtr = std::unique_ptr<PreferencesWindow>{};

    auto const globalConfigPath = std::filesystem::path{Glib::get_user_config_dir()} / "aobus" / "config.yaml";
    auto appConfigStorePtr = std::make_shared<AppConfigStore>(globalConfigPath);
    auto const layoutsDir = globalConfigPath.parent_path() / "layouts";
    auto shellLayoutStorePtr = std::make_shared<ShellLayoutStore>(layoutsDir);
    auto componentStateStorePtr = std::make_shared<ShellLayoutComponentStateStore>(layoutStateDir());

    addAppActions(appPtr, preferencesWindowPtr, appConfigStorePtr);

    appPtr->signal_activate().connect(
      [&appPtr, &mainWindowPtr, appConfigStorePtr, shellLayoutStorePtr, componentStateStorePtr]
      { handleAppActivate(appPtr, mainWindowPtr, appConfigStorePtr, shellLayoutStorePtr, componentStateStorePtr); });

    auto gtkArgv = buildGtkArgv(static_cast<std::int32_t>(args.size()), args.data());
    std::int32_t const gtkArgc = static_cast<std::int32_t>(gtkArgv.size());

    APP_LOG_INFO("Entering GTK main loop");
    auto exitCode = appPtr->run(gtkArgc, gtkArgv.data());

    if (preferencesWindowPtr)
    {
      if (preferencesWindowPtr->get_application())
      {
        appPtr->remove_window(*preferencesWindowPtr);
      }

      preferencesWindowPtr.reset();
    }

    releaseMainWindow(*appPtr, mainWindowPtr);
    return exitCode;
  }
} // namespace

int main(int argc, char* argv[])
{
  std::int32_t exitCode = 0;

  try
  {
    exitCode = runApp({argv, static_cast<std::size_t>(argc)});
  }
  catch (ao::Exception const& e)
  {
    APP_LOG_CRITICAL("Internal error: {} (at {}:{})", e.what(), e.file(), e.line());
    std::println(stderr, "Internal error: {}\n(at {}:{})\nPlease report this bug.", e.what(), e.file(), e.line());
    exitCode = 1;
  }
  catch (std::exception const& ex)
  {
    APP_LOG_CRITICAL("Unhandled exception: {}", ex.what());
    std::println(stderr, "Unhandled exception: {}", ex.what());
    exitCode = 1;
  }
  catch (...)
  {
    std::println(stderr, "Unknown unhandled exception");
    exitCode = 1;
  }

  rt::Log::shutdown();
  return exitCode;
}
