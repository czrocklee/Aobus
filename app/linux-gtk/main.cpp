// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <glibmm/main.h>

#include "app/AppConfigStore.h"
#include "app/GtkStartupPlan.h"
#include "app/GtkStyleRuntime.h"
#include "app/KeymapApplicator.h"
#include "app/LibraryWindowLifecycle.h"
#include "app/MainWindow.h"
#include "app/ShellLayoutComponentStateStore.h"
#include "app/ShellLayoutStore.h"
#include "common/MainContextCallbackScope.h"
#include "platform/SuccessorProcessLauncher.h"
#include "portal/ImportExportCoordinator.h"
#include "portal/LibraryImportExportWorkflow.h"
#include "preference/PreferencesWindow.h"
#include <ao/AppVersion.h>
#include <ao/Contract.h>
#include <ao/Error.h>
#include <ao/rt/AppPrefsState.h>
#include <ao/rt/Log.h>
#include <ao/rt/library/LibraryPaths.h>
#include <ao/uimodel/input/KeymapModel.h>
#include <ao/uimodel/preference/PreferencesEditorModel.h>
#include <ao/uimodel/preference/ThemePreset.h>
#include <ao/utility/ScopedRegistration.h>

#include <gdkmm/display.h>
#include <giomm/applaunchcontext.h>
#include <giomm/simpleaction.h>
#include <glib-unix.h>
#include <glibmm/exceptionhandler.h>
#include <glibmm/miscutils.h>
#include <glibmm/refptr.h>
#include <glibmm/variant.h>
#include <gtkmm/aboutdialog.h>
#include <gtkmm/alertdialog.h>
#include <gtkmm/application.h>

#include <array>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <expected>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace ao;
using namespace ao::gtk;

namespace
{
  constexpr auto kGtkApplicationId = "org.aobus.app";

  struct ResolvedLibraryPaths final
  {
    std::filesystem::path musicRoot;
    std::filesystem::path databasePath;
    bool scanAfterOpen = false;
  };

  struct ActivationRequest final
  {
    Glib::RefPtr<Gio::AppLaunchContext> contextPtr;
    std::optional<std::string> optToken;
  };

  struct LibraryRestartRequest final
  {
    std::filesystem::path libraryRoot;
    bool scanAfterOpen = false;
    ActivationRequest activation;
  };

  struct RunAppResult final
  {
    std::int32_t exitCode = 0;
    std::optional<LibraryRestartRequest> optRestartRequest;
    std::optional<std::string> optDiagnosticMessage;
  };

  Result<ResolvedLibraryPaths> resolveLibraryPaths(AppConfigStore const& configStore, GtkStartupPlan const& startupPlan)
  {
    if (startupPlan.optSuccessorLibraryRoot)
    {
      auto const& musicRoot = *startupPlan.optSuccessorLibraryRoot;

      if (!std::filesystem::is_directory(musicRoot))
      {
        return makeError(Error::Code::InvalidInput,
                         std::format("The selected library directory is unavailable: {}", musicRoot.string()));
      }

      return ResolvedLibraryPaths{.musicRoot = musicRoot,
                                  .databasePath = rt::LibraryPaths{musicRoot}.databasePath(),
                                  .scanAfterOpen = startupPlan.scanAfterOpen};
    }

    auto snapshot = rt::AppSessionState{};
    configStore.loadAppSession(snapshot);

    if (!snapshot.lastLibraryPath.empty())
    {
      auto musicRoot = std::filesystem::path{snapshot.lastLibraryPath};

      if (std::filesystem::exists(musicRoot))
      {
        auto const libraryPaths = rt::LibraryPaths{musicRoot};
        return ResolvedLibraryPaths{.musicRoot = musicRoot,
                                    .databasePath = libraryPaths.databasePath(),
                                    .scanAfterOpen = !libraryPaths.hasExistingDatabase()};
      }
    }

    auto const emptyPath = std::filesystem::temp_directory_path() / "aobus-empty";
    std::filesystem::create_directories(emptyPath);
    return ResolvedLibraryPaths{.musicRoot = emptyPath, .databasePath = rt::LibraryPaths{emptyPath}.databasePath()};
  }

  ActivationRequest requestDesktopActivation()
  {
    auto const displayPtr = Gdk::Display::get_default();

    if (!displayPtr)
    {
      return {};
    }

    auto contextPtr = displayPtr->get_app_launch_context();

    try
    {
      auto token = contextPtr->get_startup_notify_id({}, {});
      return {.contextPtr = std::move(contextPtr),
              .optToken = token.empty() ? std::nullopt : std::optional<std::string>{std::move(token)}};
    }
    catch (Glib::Error const& e)
    {
      APP_LOG_WARN("Failed to request a desktop activation token for library restart: {}", e.what());
      return {.contextPtr = std::move(contextPtr), .optToken = std::nullopt};
    }
  }

  void configureOpenLibraryCallback(Glib::RefPtr<MainWindow> const& windowPtr,
                                    Glib::RefPtr<Gtk::Application> const& appPtr,
                                    Glib::RefPtr<MainWindow>& mainWindowPtr,
                                    MainContextCallbackScope const& callbackScope,
                                    utility::ScopedRegistration& openLibraryIdleRegistration,
                                    std::optional<LibraryRestartRequest>& optRestartRequest);

  void handleOpenNewLibrary(std::filesystem::path const& path,
                            Glib::RefPtr<Gtk::Application> const& appPtr,
                            Glib::RefPtr<MainWindow>& mainWindowPtr,
                            std::optional<LibraryRestartRequest>& optRestartRequest,
                            bool const scanAfterOpen)
  {
    if (!mainWindowPtr || !std::filesystem::is_directory(path))
    {
      return;
    }

    auto const requestedPath = std::filesystem::absolute(path).lexically_normal();
    auto const activeRoot = std::filesystem::absolute(mainWindowPtr->musicRoot()).lexically_normal();

    if (requestedPath == activeRoot)
    {
      if (scanAfterOpen)
      {
        mainWindowPtr->importExportCoordinator().scanLibrary(portal::ScanRequestMode::FastBootstrap);
      }

      mainWindowPtr->present();
      return;
    }

    if (auto const retiredRes = mainWindowPtr->retireForLibrarySwitch(); !retiredRes)
    {
      APP_LOG_ERROR("Failed to retire the active library for process restart: {}", retiredRes.error().message);
      return;
    }

    optRestartRequest = LibraryRestartRequest{
      .libraryRoot = requestedPath, .scanAfterOpen = scanAfterOpen, .activation = requestDesktopActivation()};
    appPtr->quit();
  }

  void configureOpenLibraryCallback(Glib::RefPtr<MainWindow> const& windowPtr,
                                    Glib::RefPtr<Gtk::Application> const& appPtr,
                                    Glib::RefPtr<MainWindow>& mainWindowPtr,
                                    MainContextCallbackScope const& callbackScope,
                                    utility::ScopedRegistration& openLibraryIdleRegistration,
                                    std::optional<LibraryRestartRequest>& optRestartRequest)
  {
    windowPtr->importExportCoordinator().callbacks().onOpenNewLibrary = callbackScope.guard(
      [appPtr, &mainWindowPtr, &callbackScope, &openLibraryIdleRegistration, &optRestartRequest](
        std::filesystem::path const& path, bool const scanAfterOpen)
      {
        openLibraryIdleRegistration.reset();
        auto guardedOpen =
          callbackScope.guard([path, scanAfterOpen, appPtr, &mainWindowPtr, &optRestartRequest]
                              { handleOpenNewLibrary(path, appPtr, mainWindowPtr, optRestartRequest, scanAfterOpen); });
        auto connection = Glib::signal_idle().connect(
          [guardedOpen = std::move(guardedOpen)] mutable
          {
            guardedOpen();
            return false;
          });
        openLibraryIdleRegistration =
          utility::ScopedRegistration{[connection = std::move(connection)] mutable { connection.disconnect(); }};
      });
  }

  void releaseMainWindow(Gtk::Application& app, Glib::RefPtr<MainWindow>& mainWindowPtr)
  {
    if (!mainWindowPtr)
    {
      return;
    }

    mainWindowPtr->saveSession();

    app.remove_window(*mainWindowPtr);
    mainWindowPtr.reset();
  }

  class ProcessSignalHandlers final
  {
  public:
    ProcessSignalHandlers() = default;
    ~ProcessSignalHandlers() = default;

    ProcessSignalHandlers(ProcessSignalHandlers const&) = delete;
    ProcessSignalHandlers& operator=(ProcessSignalHandlers const&) = delete;
    ProcessSignalHandlers(ProcessSignalHandlers&&) = delete;
    ProcessSignalHandlers& operator=(ProcessSignalHandlers&&) = delete;

    void install(Glib::RefPtr<Gtk::Application> const& appPtr)
    {
      uninstall();
      _terminationRequested = false;
      _appPtr = appPtr;
      _registrations = {registerSignal(SIGINT, &ProcessSignalHandlers::handleTermination, this),
                        registerSignal(SIGTERM, &ProcessSignalHandlers::handleTermination, this),
                        registerSignal(SIGUSR1, &ProcessSignalHandlers::handleStyleReload, nullptr)};
    }

    void uninstall()
    {
      for (auto& registration : _registrations)
      {
        registration.reset();
      }

      _appPtr.reset();
    }

    bool terminationRequested() const noexcept { return _terminationRequested; }

  private:
    static ::gboolean handleTermination(void* data)
    {
      auto* const handlers = static_cast<ProcessSignalHandlers*>(data);
      handlers->_terminationRequested = true;
      APP_LOG_INFO("Received termination signal, shutting down...");

      if (auto const appPtr = handlers->_appPtr.lock(); appPtr)
      {
        appPtr->quit();
      }

      return TRUE;
    }

    static ::gboolean handleStyleReload(void* /*data*/)
    {
      APP_LOG_DEBUG("GtkStyleRuntime: Received SIGUSR1, scheduling theme refresh...");
      GtkStyleRuntime::instance().reload();
      return TRUE;
    }

    static utility::ScopedRegistration registerSignal(int const signal, GSourceFunc const handler, void* const data)
    {
      auto const sourceId = ::g_unix_signal_add(signal, handler, data);
      return utility::ScopedRegistration{
        [sourceId]
        {
          if (auto* const source = ::g_main_context_find_source_by_id(nullptr, sourceId); source != nullptr)
          {
            ::g_source_destroy(source);
          }
        }};
    }

    std::weak_ptr<Gtk::Application> _appPtr;
    std::array<utility::ScopedRegistration, 3> _registrations;
    bool _terminationRequested = false;
  };

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

  void applyThemeToMainWindows(Glib::RefPtr<Gtk::Application> const& appPtr, uimodel::ThemePreset const theme)
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
        .onApplyTheme = [appPtr](uimodel::ThemePreset const theme) { applyThemeToMainWindows(appPtr, theme); },
      });
    }

    if (!preferencesWindowPtr->get_application())
    {
      appPtr->add_window(*preferencesWindowPtr);
    }

    preferencesWindowPtr->set_transient_for(*targetWindow);
    auto prefs = rt::AppPrefsState{};
    appConfigStorePtr->loadAppPrefs(prefs);
    preferencesWindowPtr->refreshPreferences(prefs, &targetWindow->playback(), targetWindow);
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

  void removeAppActions(Gtk::Application& app)
  {
    app.set_accels_for_action("app.preferences", {});
    app.remove_action("preferences");
    app.remove_action("quit");
    app.remove_action("about");
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

  void failStartup(Glib::RefPtr<Gtk::Application> const& appPtr,
                   std::optional<std::string>& optDiagnosticMessage,
                   std::string message)
  {
    APP_LOG_CRITICAL("{}", message);
    std::println(stderr, "{}", message);
    optDiagnosticMessage = std::move(message);
    appPtr->quit();
  }

  void handleAppActivate(Glib::RefPtr<Gtk::Application>& appPtr,
                         Glib::RefPtr<MainWindow>& mainWindowPtr,
                         MainContextCallbackScope const& callbackScope,
                         utility::ScopedRegistration& openLibraryIdleRegistration,
                         std::shared_ptr<AppConfigStore> const& appConfigStorePtr,
                         std::shared_ptr<ShellLayoutStore> const& shellLayoutStorePtr,
                         std::shared_ptr<ShellLayoutComponentStateStore> const& componentStateStorePtr,
                         GtkStartupPlan const& startupPlan,
                         std::optional<LibraryRestartRequest>& optRestartRequest,
                         std::optional<std::string>& optDiagnosticMessage,
                         bool& startupCompleted)
  {
    GtkStyleRuntime::instance().initialize();

    applyKeymapAccelerators(*appPtr, appConfigStorePtr->loadKeymap(uimodel::defaultKeymap()));

    if (mainWindowPtr)
    {
      mainWindowPtr->present();
      return;
    }

    auto pathsRes = resolveLibraryPaths(*appConfigStorePtr, startupPlan);

    if (!pathsRes)
    {
      failStartup(appPtr,
                  optDiagnosticMessage,
                  std::format("Aobus could not select the startup library: {}", pathsRes.error().message));
      return;
    }

    auto paths = std::move(*pathsRes);

    auto const scanAfterOpen = paths.scanAfterOpen;
    auto windowRes =
      prepareLibraryWindow({.musicRoot = std::move(paths.musicRoot), .databasePath = std::move(paths.databasePath)},
                           appConfigStorePtr,
                           shellLayoutStorePtr,
                           componentStateStorePtr);

    if (!windowRes)
    {
      failStartup(
        appPtr, optDiagnosticMessage, std::format("Aobus could not open the library: {}", windowRes.error().message));
      return;
    }

    mainWindowPtr = std::move(*windowRes);
    configureOpenLibraryCallback(
      mainWindowPtr, appPtr, mainWindowPtr, callbackScope, openLibraryIdleRegistration, optRestartRequest);

    auto const restoreMode = startupPlan.optSuccessorLibraryRoot ? MainWindow::PlaybackRestoreMode::StartIdle
                                                                 : MainWindow::PlaybackRestoreMode::Restore;

    if (auto const activatedRes = activateLibraryWindow(*appPtr, mainWindowPtr, restoreMode); !activatedRes)
    {
      mainWindowPtr.reset();
      failStartup(appPtr,
                  optDiagnosticMessage,
                  std::format("Aobus could not activate the library: {}", activatedRes.error().message));
      return;
    }

    if (startupPlan.optSuccessorLibraryRoot)
    {
      if (auto const persistedRes = mainWindowPtr->commitSuccessorLibrarySelection(); !persistedRes)
      {
        APP_LOG_WARN("Failed to persist the selected GTK library path: {}", persistedRes.error().message);
      }
    }

    startupCompleted = true;

    if (scanAfterOpen)
    {
      mainWindowPtr->importExportCoordinator().scanLibrary(portal::ScanRequestMode::FastBootstrap);
    }
  }

  // CLI11 and GTK both expose the process entry-point's mutable C argv array.
  struct MutableArguments final
  {
    std::vector<std::string> strings;
    std::vector<char*> pointers;
  };

  MutableArguments buildMutableArgv(std::vector<std::string> arguments)
  {
    auto mutableArguments = MutableArguments{.strings = std::move(arguments), .pointers = {}};
    mutableArguments.pointers.reserve(mutableArguments.strings.size() + 1);

    for (auto& argument : mutableArguments.strings)
    {
      mutableArguments.pointers.push_back(argument.data());
    }

    mutableArguments.pointers.push_back(nullptr);

    return mutableArguments;
  }

  void handleSignalException()
  {
    AO_FATAL_EXCEPTION(std::current_exception(), "GTK signal handler");
  }

  RunAppResult runApp(std::span<char*> args, ProcessSignalHandlers& processSignalHandlers)
  {
    auto argumentViews = std::vector<std::string_view>{};
    argumentViews.reserve(args.size());

    for (auto const* const argument : args)
    {
      argumentViews.emplace_back(argument);
    }

    auto startupPlanRes = planGtkStartup(argumentViews);

    if (!startupPlanRes)
    {
      std::println(stderr, "Aobus could not plan GTK startup: {}", startupPlanRes.error().message);
      return {.exitCode = EXIT_FAILURE, .optRestartRequest = std::nullopt, .optDiagnosticMessage = std::nullopt};
    }

    auto startupPlan = std::move(*startupPlanRes);

    if (startupPlan.shouldExit)
    {
      if (startupPlan.showVersion)
      {
        std::println("Aobus {}", kAppVersion);
      }

      return {
        .exitCode = startupPlan.exitCode, .optRestartRequest = std::nullopt, .optDiagnosticMessage = std::nullopt};
    }

    auto const logDir = std::filesystem::path{Glib::get_user_cache_dir()} / "aobus" / "logs";
    rt::Log::initialize(startupPlan.logLevel, logDir);

    APP_LOG_INFO("Aobus {} starting...", kAppVersion);

    Glib::set_application_name("Aobus");

    auto applicationFlags = Gio::Application::Flags::ALLOW_REPLACEMENT;

    if (startupPlan.registrationMode == GtkApplicationRegistrationMode::ReplaceExisting)
    {
      applicationFlags |= Gio::Application::Flags::REPLACE;
    }

    auto appPtr = Gtk::Application::create(std::string{kGtkApplicationId}, applicationFlags);
    processSignalHandlers.install(appPtr);

    // Top-level boundary for exceptions that escape a GTK signal/action handler.
    // Such exceptions must not unwind through glib's C frames. glibmm catches
    // them at the slot boundary; the project fatal root adds owned context.
    Glib::add_exception_handler([] { handleSignalException(); });

    auto mainWindowPtr = Glib::RefPtr<MainWindow>{};
    auto preferencesWindowPtr = std::unique_ptr<PreferencesWindow>{};
    auto optRestartRequest = std::optional<LibraryRestartRequest>{};
    auto optDiagnosticMessage = std::optional<std::string>{};
    bool startupCompleted = false;

    auto const globalConfigPath = std::filesystem::path{Glib::get_user_config_dir()} / "aobus" / "config.yaml";
    auto appConfigStorePtr = std::make_shared<AppConfigStore>(globalConfigPath);
    auto const layoutsDir = globalConfigPath.parent_path() / "layouts";
    auto shellLayoutStorePtr = std::make_shared<ShellLayoutStore>(layoutsDir);
    auto componentStateStorePtr = std::make_shared<ShellLayoutComponentStateStore>(layoutStateDir());

    // Preserve reverse-destruction order: application signals/actions close
    // first, then callback admission and the idle source, then windows and
    // their attached runtime, style, stores, and finally Gtk::Application.
    auto styleRuntimeRegistration = utility::ScopedRegistration{[] { GtkStyleRuntime::instance().shutdown(); }};
    auto windowRegistration =
      utility::ScopedRegistration{[&appPtr, &mainWindowPtr, &preferencesWindowPtr]
                                  {
                                    auto exceptionPtr = std::exception_ptr{};

                                    try
                                    {
                                      if (preferencesWindowPtr)
                                      {
                                        if (preferencesWindowPtr->get_application())
                                        {
                                          appPtr->remove_window(*preferencesWindowPtr);
                                        }

                                        preferencesWindowPtr.reset();
                                      }
                                    }
                                    catch (...)
                                    {
                                      exceptionPtr = std::current_exception();
                                    }

                                    try
                                    {
                                      releaseMainWindow(*appPtr, mainWindowPtr);
                                    }
                                    catch (...)
                                    {
                                      if (!exceptionPtr)
                                      {
                                        exceptionPtr = std::current_exception();
                                      }
                                    }

                                    if (exceptionPtr)
                                    {
                                      AO_FATAL_EXCEPTION(std::move(exceptionPtr), "GTK window shutdown");
                                    }
                                  }};
    auto openLibraryIdleRegistration = utility::ScopedRegistration{};
    auto callbackScope =
      MainContextCallbackScope{[&openLibraryIdleRegistration] { openLibraryIdleRegistration.reset(); }};

    addAppActions(appPtr, preferencesWindowPtr, appConfigStorePtr);
    auto appActionsRegistration = utility::ScopedRegistration{[appPtr] { removeAppActions(*appPtr); }};

    auto activateConnection = appPtr->signal_activate().connect(
      [&appPtr,
       &mainWindowPtr,
       &callbackScope,
       &openLibraryIdleRegistration,
       appConfigStorePtr,
       shellLayoutStorePtr,
       componentStateStorePtr,
       &startupPlan,
       &optRestartRequest,
       &optDiagnosticMessage,
       &startupCompleted]
      {
        handleAppActivate(appPtr,
                          mainWindowPtr,
                          callbackScope,
                          openLibraryIdleRegistration,
                          appConfigStorePtr,
                          shellLayoutStorePtr,
                          componentStateStorePtr,
                          startupPlan,
                          optRestartRequest,
                          optDiagnosticMessage,
                          startupCompleted);
      });
    auto activateRegistration =
      utility::ScopedRegistration{[connection = std::move(activateConnection)] mutable { connection.disconnect(); }};

    auto gtkArguments = buildMutableArgv(std::move(startupPlan.gtkArguments));
    std::int32_t const gtkArgc = static_cast<std::int32_t>(gtkArguments.strings.size());

    APP_LOG_INFO("Entering GTK main loop");
    auto const exitCode = appPtr->run(gtkArgc, gtkArguments.pointers.data());

    if (!optDiagnosticMessage && !processSignalHandlers.terminationRequested())
    {
      optDiagnosticMessage =
        incompleteSuccessorStartupDiagnostic(startupPlan.registrationMode, startupCompleted, exitCode);

      if (optDiagnosticMessage)
      {
        APP_LOG_CRITICAL("{}", *optDiagnosticMessage);
        std::println(stderr, "{}", *optDiagnosticMessage);
      }
    }

    return {.exitCode = optDiagnosticMessage ? EXIT_FAILURE : exitCode,
            .optRestartRequest = std::move(optRestartRequest),
            .optDiagnosticMessage = std::move(optDiagnosticMessage)};
  }

  std::int32_t runDiagnosticApp(std::string const& message, ProcessSignalHandlers& processSignalHandlers)
  {
    auto appPtr = Gtk::Application::create({}, Gio::Application::Flags::NON_UNIQUE);
    processSignalHandlers.install(appPtr);

    auto diagnosticActivateConnection = appPtr->signal_activate().connect(
      [appPtr, message]
      {
        auto alertPtr = Gtk::AlertDialog::create("Aobus Startup Failed");
        alertPtr->set_detail(message);
        appPtr->hold();
        alertPtr->choose(
          [alertPtr, appPtr](Glib::RefPtr<Gio::AsyncResult> const& resultPtr)
          {
            try
            {
              std::ignore = alertPtr->choose_finish(resultPtr);
            }
            catch (Glib::Error const&) // NOLINT(bugprone-empty-catch) -- Closing the native dialog is expected.
            {
              // Closing or cancelling the standalone diagnostic is expected.
            }

            appPtr->release();
          });
      });

    auto const exitCode = appPtr->run();
    diagnosticActivateConnection.disconnect();
    processSignalHandlers.uninstall();
    return exitCode;
  }
} // namespace

int main(int argc, char* argv[])
{
  Glib::set_prgname("aobus");
  auto processSignalHandlers = ProcessSignalHandlers{};

  try
  {
    auto result = runApp({argv, static_cast<std::size_t>(argc)}, processSignalHandlers);
    processSignalHandlers.uninstall();

    if (result.optRestartRequest)
    {
      // Reaching the caller proves every local in runApp's GTK composition
      // scope has completed destruction; process creation must stay below
      // this boundary so parent and successor library graphs cannot overlap.
      auto& request = *result.optRestartRequest;
      auto const optToken =
        request.activation.optToken ? std::optional<std::string_view>{*request.activation.optToken} : std::nullopt;
      auto const launchedRes = launchDetachedSuccessor(request.libraryRoot, request.scanAfterOpen, optToken);

      if (!launchedRes)
      {
        if (request.activation.contextPtr && request.activation.optToken)
        {
          request.activation.contextPtr->launch_failed(*request.activation.optToken);
        }

        auto const message = std::format("Aobus could not start the selected library: {}", launchedRes.error().message);
        APP_LOG_ERROR("{}", message);
        std::ignore = runDiagnosticApp(message, processSignalHandlers);
        rt::Log::shutdown();
        return EXIT_FAILURE;
      }
    }
    else if (result.optDiagnosticMessage)
    {
      std::ignore = runDiagnosticApp(*result.optDiagnosticMessage, processSignalHandlers);
    }

    rt::Log::shutdown();
    return result.exitCode;
  }
  catch (...)
  {
    AO_FATAL_EXCEPTION(std::current_exception(), "GTK process root");
  }
}
