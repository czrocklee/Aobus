// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/MainWindow.h"

#include "app/AppConfigStore.h"
#include "app/AppDialog.h"
#include "app/GtkMainContextExecutor.h"
#include "app/LibraryWindowLifecycle.h"
#include "app/ShellLayoutStore.h"
#include "app/WindowState.h"
#include "i18n/GtkTextCatalog.h"
#include "list/ListNavigationController.h"
#include "runtime/PlaybackSessionState.h"
#include "runtime/PlaybackSessionYamlSchema.h"
#include "test/unit/MessageCatalogTestSupport.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include "test/unit/library/MusicLibraryTestSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/linux-gtk/GtkApplicationTestSupport.h"
#include "test/unit/linux-gtk/GtkRuntimeTestSupport.h"
#include "test/unit/linux-gtk/GtkWidgetTestSupport.h"
#include "test/unit/runtime/AppRuntimeTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/Device.h>
#include <ao/audio/Transport.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/library/MusicLibrary.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/AppState.h>
#include <ao/rt/ConfigStore.h>
#include <ao/rt/ListMutation.h>
#include <ao/rt/PlaybackMode.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryCommands.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/rt/playback/PlaybackSnapshot.h>
#include <ao/uimodel/preference/ThemePreset.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <giomm/actionmap.h>
#include <gtkmm/dialog.h>
#include <gtkmm/menubutton.h>
#include <gtkmm/popovermenubar.h>
#include <gtkmm/window.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::gtk::test
{
  TEST_CASE("MainWindow - constructs shell with title and window actions", "[gtk][unit][main-window]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};

    auto const configPath = std::filesystem::path{fixture.tempDir().path()} / "app_config.yaml";
    auto configStorePtr = std::make_shared<AppConfigStore>(configPath);
    configStorePtr->saveWindow(WindowState{.width = 640, .height = 480, .maximized = false});

    auto window = MainWindow{fixture.runtime(), configStorePtr, nullptr, ao::test::englishMessageCatalog()};

    CHECK(window.get_title() == "Aobus");

    std::int32_t defaultWidth = 0;
    std::int32_t defaultHeight = 0;
    window.get_default_size(defaultWidth, defaultHeight);
    CHECK(defaultWidth == 640);
    CHECK(defaultHeight == 480);

    auto* const actionMap = dynamic_cast<Gio::ActionMap*>(&window);
    REQUIRE(actionMap != nullptr);
    CHECK(actionMap->lookup_action("open-library") != nullptr);
    CHECK(actionMap->lookup_action("scan-library") != nullptr);
    CHECK(actionMap->lookup_action("import-library") != nullptr);
    CHECK(actionMap->lookup_action("export-library") != nullptr);
    CHECK(actionMap->lookup_action("edit-layout") != nullptr);
    CHECK(actionMap->lookup_action("reset-runtime-layout-state") != nullptr);
    CHECK(actionMap->lookup_action("save-panel-sizes-as-layout-defaults") != nullptr);
    CHECK(actionMap->lookup_action("keyboard-shortcuts") == nullptr);
    CHECK(actionMap->lookup_action("list-new-smart-list") != nullptr);
    CHECK(actionMap->lookup_action("list-edit") != nullptr);
    CHECK(actionMap->lookup_action("list-delete") != nullptr);
  }

  TEST_CASE("MainWindow - hide persists current library path", "[gtk][unit][main-window]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};

    auto const configPath = std::filesystem::path{fixture.tempDir().path()} / "app_config.yaml";
    auto configStorePtr = std::make_shared<AppConfigStore>(configPath);

    auto window = MainWindow{fixture.runtime(), configStorePtr, nullptr, ao::test::englishMessageCatalog()};
    REQUIRE(window.prepareSession());
    REQUIRE(window.activateSession(MainWindow::PlaybackRestoreMode::Restore));

    auto before = rt::AppSessionState{};
    configStorePtr->loadAppSession(before);
    REQUIRE(before.lastLibraryPath.empty());

    window.present();
    window.hide();

    auto after = rt::AppSessionState{};
    configStorePtr->loadAppSession(after);
    CHECK(after.lastLibraryPath == fixture.runtime().musicRoot().string());
  }

  TEST_CASE("MainWindow - explicit session save persists current library path", "[gtk][unit][main-window]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};

    auto const configPath = std::filesystem::path{fixture.tempDir().path()} / "app_config.yaml";
    auto configStorePtr = std::make_shared<AppConfigStore>(configPath);

    auto window = MainWindow{fixture.runtime(), configStorePtr, nullptr, ao::test::englishMessageCatalog()};
    REQUIRE(window.prepareSession());
    REQUIRE(window.activateSession(MainWindow::PlaybackRestoreMode::Restore));

    auto before = rt::AppSessionState{};
    configStorePtr->loadAppSession(before);
    REQUIRE(before.lastLibraryPath.empty());

    window.saveSession();

    auto after = rt::AppSessionState{};
    configStorePtr->loadAppSession(after);
    CHECK(after.lastLibraryPath == fixture.runtime().musicRoot().string());
  }

  TEST_CASE("MainWindow - library switch forgets playback and prevents stale path writes",
            "[gtk][unit][main-window][session]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();

    auto const configPath = std::filesystem::path{fixture.tempDir().path()} / "app_config.yaml";
    auto configStorePtr = std::make_shared<AppConfigStore>(configPath);
    REQUIRE(runtime.playbackSessionConfigStore().save(
      rt::kPlaybackSessionConfigGroup, rt::PlaybackSessionState{}, rt::PlaybackSessionYamlSchema{}));

    auto window = MainWindow{runtime, configStorePtr, nullptr, ao::test::englishMessageCatalog()};
    REQUIRE(window.prepareSession());
    REQUIRE(window.activateSession(MainWindow::PlaybackRestoreMode::StartIdle));
    REQUIRE(window.retireForLibrarySwitch());
    CHECK(window.sessionPhase() == MainWindow::SessionPhase::Retired);
    CHECK_FALSE(*runtime.playbackSessionConfigStore().contains(rt::kPlaybackSessionConfigGroup));

    auto switchedSession = rt::AppSessionState{};
    switchedSession.lastLibraryPath = "/tmp/new-library";
    REQUIRE(configStorePtr->saveAppSession(switchedSession));
    window.saveSession();

    auto persistedSession = rt::AppSessionState{};
    configStorePtr->loadAppSession(persistedSession);
    CHECK(persistedSession.lastLibraryPath == "/tmp/new-library");
  }

  TEST_CASE("MainWindow - failed successor root commit isolates root and playback persistence",
            "[gtk][regression][main-window][concurrency]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto tempDir = ao::test::TempDir{};
    auto const musicRoot = tempDir.path() / "music";
    auto const databasePath = tempDir.path() / "database";
    auto const configDir = tempDir.path() / "global-config";
    auto const parkedConfigDir = tempDir.path() / "parked-global-config";
    auto const configPath = configDir / "config.yaml";
    auto const workspacePath = tempDir.path() / "workspace.yaml";
    auto const layoutPath = musicRoot / ".aobus" / "gtk_layout.yaml";
    std::filesystem::create_directories(musicRoot);
    std::filesystem::create_directories(databasePath);
    std::filesystem::create_directories(configDir);

    auto configStorePtr = std::make_shared<AppConfigStore>(configPath);
    auto oldSession = rt::AppSessionState{};
    oldSession.lastLibraryPath = "/old/durable/library";
    oldSession.lastOutputSelection.backendId = audio::BackendId{"old-backend"};
    oldSession.lastOutputSelection.deviceId = audio::DeviceId{"old-device"};
    oldSession.lastOutputSelection.profileId = audio::ProfileId{"old-profile"};
    REQUIRE(configStorePtr->saveAppSession(oldSession));
    REQUIRE_FALSE(*configStorePtr->playbackSessionStore().contains(rt::kPlaybackSessionConfigGroup));

    auto runtimePtr = ao::test::requireValue(rt::AppRuntime::create(rt::AppRuntimeDependencies{
      .executorPtr = std::make_unique<GtkMainContextExecutor>(),
      .musicRoot = musicRoot,
      .databasePath = databasePath,
      .musicLibraryPinnedMapBytes = library::test::kTestMusicLibraryMapBytes,
      .workspaceConfigStorePtr = std::make_unique<rt::ConfigStore>(workspacePath),
      .playbackSessionConfigStore = &configStorePtr->playbackSessionStore(),
    }));
    rt::test::addReadyAudioProvider(*runtimePtr);
    drainGtkEvents();

    auto const fixturePath = audio::test::requireAudioFixture("basic_metadata.flac").string();
    auto const trackId = addRuntimeTrack(*runtimePtr, {.title = "Successor Track", .uri = fixturePath});

    {
      auto window = MainWindow{*runtimePtr, configStorePtr, nullptr, ao::test::englishMessageCatalog()};
      REQUIRE(window.prepareSession());
      REQUIRE(window.activateSession(MainWindow::PlaybackRestoreMode::StartIdle));
      auto const viewId = runtimePtr->workspace().snapshot().activeViewId;
      REQUIRE(viewId != rt::kInvalidViewId);
      runtimePtr->playback().commands().setOutputDevice(
        audio::BackendId{"test_backend"}, audio::DeviceId{"test_device"}, audio::kProfileShared);
      drainGtkEvents();
      std::filesystem::remove(workspacePath);
      std::filesystem::remove(layoutPath);

      std::filesystem::rename(configDir, parkedConfigDir);

      {
        auto blocker = std::ofstream{configDir};
        REQUIRE(blocker);
        blocker << "not a directory";
      }

      auto const committedRes = window.commitSuccessorLibrarySelection();
      REQUIRE_FALSE(committedRes);
      CHECK(committedRes.error().code == Error::Code::IoError);

      REQUIRE(std::filesystem::remove(configDir));
      std::filesystem::rename(parkedConfigDir, configDir);

      auto durableSession = rt::AppSessionState{};
      configStorePtr->loadAppSession(durableSession);
      CHECK(durableSession.lastLibraryPath == oldSession.lastLibraryPath);
      REQUIRE_FALSE(*configStorePtr->playbackSessionStore().contains(rt::kPlaybackSessionConfigGroup));

      SECTION("playback stays blocked while ordinary window state saves")
      {
        REQUIRE(runtimePtr->playback().commands().startFromView(viewId, trackId));
        REQUIRE(waitForPlaybackSettlement(*runtimePtr, trackId));
        runtimePtr->playback().commands().pause();
        drainGtkEvents();
        CHECK_FALSE(*configStorePtr->playbackSessionStore().contains(rt::kPlaybackSessionConfigGroup));

        REQUIRE(runtimePtr->savePlaybackSession());
        CHECK_FALSE(*configStorePtr->playbackSessionStore().contains(rt::kPlaybackSessionConfigGroup));

        window.saveSession();
        window.present();
        window.hide();
        drainGtkEvents();
        configStorePtr->loadAppSession(durableSession);
        CHECK(durableSession.lastLibraryPath == oldSession.lastLibraryPath);
        CHECK(durableSession.lastOutputSelection.backendId == "test_backend");
        CHECK(durableSession.lastOutputSelection.deviceId == "test_device");
        CHECK(durableSession.lastOutputSelection.profileId == audio::kProfileShared.raw());
        REQUIRE(*configStorePtr->playbackSessionStore().contains("window"));
        CHECK(std::filesystem::exists(workspacePath));
        CHECK(std::filesystem::exists(layoutPath));
        CHECK_FALSE(*configStorePtr->playbackSessionStore().contains(rt::kPlaybackSessionConfigGroup));
      }

      SECTION("a later library switch retries durable payload removal")
      {
        REQUIRE(configStorePtr->playbackSessionStore().save(
          rt::kPlaybackSessionConfigGroup, rt::PlaybackSessionState{}, rt::PlaybackSessionYamlSchema{}));
        REQUIRE(window.retireForLibrarySwitch());
        CHECK(window.sessionPhase() == MainWindow::SessionPhase::Retired);
        CHECK_FALSE(*configStorePtr->playbackSessionStore().contains(rt::kPlaybackSessionConfigGroup));
      }
    }

    runtimePtr->shutdown();
    runtimePtr->shutdown();
    drainGtkEvents();
    CHECK_FALSE(*configStorePtr->playbackSessionStore().contains(rt::kPlaybackSessionConfigGroup));
  }

  TEST_CASE("MainWindow - failed library preparation stays visible and keeps the active window usable",
            "[gtk][regression][main-window][session]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto tempDir = ao::test::TempDir{};
    auto const musicRoot = tempDir.path() / "music";
    auto const databasePath = tempDir.path() / "database";
    auto const invalidPlaybackStorePath = tempDir.path() / "playback-store-directory";
    std::filesystem::create_directories(musicRoot);
    std::filesystem::create_directories(databasePath);
    std::filesystem::create_directories(invalidPlaybackStorePath);
    auto invalidPlaybackStore = rt::ConfigStore{invalidPlaybackStorePath};
    auto runtimePtr = ao::test::requireValue(rt::AppRuntime::create(rt::AppRuntimeDependencies{
      .executorPtr = std::make_unique<GtkMainContextExecutor>(),
      .musicRoot = musicRoot,
      .databasePath = databasePath,
      .musicLibraryPinnedMapBytes = library::test::kTestMusicLibraryMapBytes,
      .workspaceConfigStorePtr = std::make_unique<rt::ConfigStore>(tempDir.path() / "workspace.yaml"),
      .playbackSessionConfigStore = &invalidPlaybackStore,
    }));
    auto configStorePtr = std::make_shared<AppConfigStore>(tempDir.path() / "app-config.yaml");
    auto window = MainWindow{*runtimePtr, configStorePtr, nullptr, ao::test::englishMessageCatalog()};
    REQUIRE(window.prepareSession());
    REQUIRE(window.activateSession(MainWindow::PlaybackRestoreMode::Restore));
    window.applyTheme(uimodel::ThemePreset::Modern);

    auto const retiredRes = window.retireForLibrarySwitch();

    REQUIRE_FALSE(retiredRes);
    CHECK(retiredRes.error().code == Error::Code::IoError);
    CHECK(window.sessionPhase() == MainWindow::SessionPhase::Active);
    CHECK(window.musicRoot() == runtimePtr->musicRoot());
    CHECK_NOTHROW(window.playback());

    window.saveSession();
    auto persistedSession = rt::AppSessionState{};
    configStorePtr->loadAppSession(persistedSession);
    CHECK(persistedSession.lastLibraryPath == runtimePtr->musicRoot().string());
    AppDialog* errorDialog = nullptr;

    for (auto* const topLevel : Gtk::Window::list_toplevels())
    {
      if (auto* const dialog = dynamic_cast<AppDialog*>(topLevel);
          dialog != nullptr && dialog->get_title() == "Unable to Switch Libraries")
      {
        errorDialog = dialog;
        break;
      }
    }

    REQUIRE(errorDialog != nullptr);
    CHECK(errorDialog->get_transient_for() == &window);
    CHECK(errorDialog->has_css_class("ao-theme-modern"));
    window.applyTheme(uimodel::ThemePreset::Classic);
    CHECK_FALSE(errorDialog->has_css_class("ao-theme-modern"));
    CHECK(errorDialog->has_css_class("ao-theme-classic"));
    errorDialog->response(Gtk::ResponseType::CLOSE);
    drainGtkEvents();
  }

  TEST_CASE("MainWindow - restores saved output when audio provider is bootstrapped before session load",
            "[gtk][unit][main-window][audio]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};

    auto const configPath = std::filesystem::path{fixture.tempDir().path()} / "app_config.yaml";
    auto configStorePtr = std::make_shared<AppConfigStore>(configPath);
    auto prefs = rt::AppPrefsState{};
    prefs.preferredOutputSelection.backendId = audio::BackendId{"test_backend"};
    prefs.preferredOutputSelection.deviceId = audio::DeviceId{"test_device"};
    prefs.preferredOutputSelection.profileId = audio::kProfileShared;
    configStorePtr->saveAppPrefs(prefs);

    rt::test::addReadyAudioProvider(fixture.runtime());

    auto window = MainWindow{fixture.runtime(), configStorePtr, nullptr, ao::test::englishMessageCatalog()};
    drainGtkEvents();

    auto const output = fixture.runtime().playback().snapshot().transport.output.selectedDevice;
    CHECK(output.backendId == audio::BackendId{"test_backend"});
    CHECK(output.deviceId == audio::DeviceId{"test_device"});
    CHECK(output.profileId == audio::kProfileShared);
  }

  TEST_CASE("MainWindow - prepared session remains isolated until activation", "[gtk][unit][main-window][session]")
  {
    auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto const configPath = std::filesystem::path{fixture.tempDir().path()} / "app_config.yaml";
    auto const workspacePath = std::filesystem::path{fixture.tempDir().path()} / "config.yaml";
    auto configStorePtr = std::make_shared<AppConfigStore>(configPath);

    {
      auto window = MainWindow{fixture.runtime(), configStorePtr, nullptr, ao::test::englishMessageCatalog()};
      REQUIRE(window.prepareSession());

      CHECK(window.sessionPhase() == MainWindow::SessionPhase::Prepared);
      CHECK_FALSE(window.isMprisStarted());
      CHECK_FALSE(window.get_application());
      CHECK_FALSE(std::filesystem::exists(workspacePath));
      window.saveSession();
      window.hide();
    }

    auto session = rt::AppSessionState{};
    configStorePtr->loadAppSession(session);
    CHECK(session.lastLibraryPath.empty());
    CHECK_FALSE(std::filesystem::exists(workspacePath));
  }

  TEST_CASE("prepareLibraryWindow - prepared window activates once and finalizes after retirement",
            "[gtk][regression][active-library]")
  {
    auto const appPtr = ensureGtkApplication();
    REQUIRE(appPtr->register_application());
    auto tempDir = ao::test::TempDir{};
    auto const musicRoot = tempDir.path() / "music";
    auto const databasePath = tempDir.path() / "database";
    std::filesystem::create_directories(musicRoot);
    auto configStorePtr = std::make_shared<AppConfigStore>(tempDir.path() / "app-config.yaml");
    bool finalized = false;

    auto windowRes = prepareLibraryWindow({.musicRoot = musicRoot, .databasePath = databasePath},
                                          configStorePtr,
                                          nullptr,
                                          nullptr,
                                          ao::test::englishMessageCatalog());
    REQUIRE(windowRes);
    auto windowPtr = std::move(*windowRes);
    ::g_object_weak_ref(
      G_OBJECT(windowPtr->gobj()), [](gpointer data, GObject*) { *static_cast<bool*>(data) = true; }, &finalized);

    CHECK(windowPtr->sessionPhase() == MainWindow::SessionPhase::Prepared);
    CHECK_FALSE(windowPtr->get_application());
    CHECK_FALSE(windowPtr->isMprisStarted());

    REQUIRE(activateLibraryWindow(*appPtr, windowPtr, MainWindow::PlaybackRestoreMode::StartIdle));

    CHECK(windowPtr->sessionPhase() == MainWindow::SessionPhase::Active);
    CHECK(windowPtr->get_application() == appPtr);
    CHECK(windowPtr->isMprisStarted());

    REQUIRE(windowPtr->retireForLibrarySwitch());
    windowPtr->close();
    drainGtkEvents();

    if (windowPtr->get_application())
    {
      appPtr->remove_window(*windowPtr);
    }

    windowPtr.reset();
    drainGtkEvents();

    CHECK(finalized);
  }

  TEST_CASE("MainWindow - shell menu components receive the window's menu model",
            "[gtk][regression][main-window][menu]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};

    auto const configPath = std::filesystem::path{fixture.tempDir().path()} / "app_config.yaml";
    auto configStorePtr = std::make_shared<AppConfigStore>(configPath);

    auto const presetId = GENERATE(std::string_view{"classic"}, std::string_view{"modern"});
    auto prefs = rt::AppPrefsState{};
    prefs.lastLayoutPreset = std::string{presetId};
    configStorePtr->saveAppPrefs(prefs);

    auto layoutStorePtr =
      std::make_shared<ShellLayoutStore>(std::filesystem::path{fixture.tempDir().path()} / "layouts");
    auto window =
      MainWindow{fixture.runtime(), configStorePtr, std::move(layoutStorePtr), ao::test::englishMessageCatalog()};
    REQUIRE(window.prepareSession());

    auto* const shell = window.get_child();
    REQUIRE(shell != nullptr);

    // The menu model is a shell-lifetime collaborator captured when the layout components are
    // registered, so a window that builds its menu too late leaves every menu surface empty.
    // The modern preset's application menu is one of several Gtk::MenuButton widgets in the tree,
    // so it is identified by the tooltip the component gives it.
    auto const applicationMenuLabel =
      gtkText(ao::test::englishMessageCatalog(), i18n::MessageId::GtkShellApplicationMenu);
    auto const findApplicationMenu = [shell, &applicationMenuLabel] -> Gtk::Widget*
    {
      if (auto* const menuBar = findWidget<Gtk::PopoverMenuBar>(*shell); menuBar != nullptr)
      {
        return menuBar;
      }

      for (auto* const button : collectAll<Gtk::MenuButton>(*shell))
      {
        if (button->get_tooltip_text() == applicationMenuLabel)
        {
          return button;
        }
      }

      return nullptr;
    };

    REQUIRE(pumpGtkEventsUntil([&findApplicationMenu] { return findApplicationMenu() != nullptr; }));

    auto* const applicationMenu = findApplicationMenu();
    REQUIRE(applicationMenu != nullptr);

    if (auto* const menuBar = dynamic_cast<Gtk::PopoverMenuBar*>(applicationMenu); menuBar != nullptr)
    {
      CHECK(menuBar->get_menu_model() != nullptr);
    }
    else
    {
      CHECK(dynamic_cast<Gtk::MenuButton&>(*applicationMenu).get_menu_model() != nullptr);
    }
  }

  TEST_CASE("MainWindow - a session checkpoint does not clobber explicit preferences",
            "[gtk][unit][main-window][config]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto const configPath = std::filesystem::path{fixture.tempDir().path()} / "app_config.yaml";
    auto configStorePtr = std::make_shared<AppConfigStore>(configPath);

    auto initialPrefs = rt::AppPrefsState{};
    initialPrefs.lastThemePreset = "modern";
    initialPrefs.preferredOutputSelection.backendId = audio::BackendId{"preference-backend"};
    initialPrefs.preferredOutputSelection.deviceId = audio::DeviceId{"preference-device"};
    initialPrefs.preferredOutputSelection.profileId = audio::ProfileId{"preference-profile"};
    configStorePtr->saveAppPrefs(initialPrefs);

    auto window = MainWindow{runtime, configStorePtr, nullptr, ao::test::englishMessageCatalog()};
    REQUIRE(window.prepareSession());
    REQUIRE(window.activateSession(MainWindow::PlaybackRestoreMode::Restore));

    // Applying a theme is a preference the user set in this session; a window
    // checkpoint records the session, not a second opinion about preferences.
    window.applyTheme(uimodel::ThemePreset::Classic);
    window.saveSession();

    auto loadedPrefs = rt::AppPrefsState{};
    configStorePtr->loadAppPrefs(loadedPrefs);
    CHECK(loadedPrefs.lastThemePreset == "modern");
    CHECK(loadedPrefs.preferredOutputSelection.backendId == "preference-backend");
    CHECK(loadedPrefs.preferredOutputSelection.deviceId == "preference-device");
    CHECK(loadedPrefs.preferredOutputSelection.profileId == "preference-profile");

    auto loadedSession = rt::AppSessionState{};
    configStorePtr->loadAppSession(loadedSession);
    CHECK(loadedSession.lastLibraryPath == runtime.musicRoot().string());
  }

  TEST_CASE("MainWindow - rejected workspace state is not overwritten during preparation",
            "[gtk][unit][main-window][workspace]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto const workspacePath = std::filesystem::path{fixture.tempDir().path()} / "config.yaml";
    auto const rejected = std::string{"workspace:\n"
                                      "  presentationVersion: 2\n"
                                      "  openViews: []\n"
                                      "  activeViewIndex: 0\n"
                                      "  customPresets: []\n"};
    std::ofstream{workspacePath} << rejected;
    auto const appConfigPath = std::filesystem::path{fixture.tempDir().path()} / "app_config.yaml";
    auto configStorePtr = std::make_shared<AppConfigStore>(appConfigPath);
    auto window = MainWindow{runtime, configStorePtr, nullptr, ao::test::englishMessageCatalog()};

    REQUIRE(window.prepareSession());

    // A workspace document this build cannot read still belongs to the user:
    // the window opens a default view beside it rather than replacing it.
    CHECK(ao::test::readFile(workspacePath) == rejected);
    CHECK(runtime.workspace().snapshot().openViews.size() == 1);
    CHECK(runtime.workspace().snapshot().activeViewId != rt::kInvalidViewId);
  }

  TEST_CASE("MainWindow - partial output preferences fall back to the session output",
            "[gtk][unit][main-window][audio]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    rt::test::addReadyAudioProvider(runtime);
    auto const configPath = std::filesystem::path{fixture.tempDir().path()} / "app_config.yaml";
    auto configStorePtr = std::make_shared<AppConfigStore>(configPath);

    auto prefs = rt::AppPrefsState{};
    prefs.preferredOutputSelection.backendId = audio::BackendId{"incomplete-preference-backend"};
    prefs.preferredOutputSelection.profileId = {};
    configStorePtr->saveAppPrefs(prefs);

    auto session = rt::AppSessionState{};
    session.lastOutputSelection.backendId = audio::BackendId{"test_backend"};
    session.lastOutputSelection.deviceId = audio::DeviceId{"test_device"};
    session.lastOutputSelection.profileId = audio::kProfileShared;
    REQUIRE(configStorePtr->saveAppSession(session));

    auto window = MainWindow{runtime, configStorePtr, nullptr, ao::test::englishMessageCatalog()};
    drainGtkEvents();

    auto const selected = runtime.playback().snapshot().transport.output.selectedDevice;
    CHECK(selected.backendId == audio::BackendId{"test_backend"});
    CHECK(selected.deviceId == audio::DeviceId{"test_device"});
    CHECK(selected.profileId == audio::kProfileShared);
  }

  TEST_CASE("MainWindow - restores a playback session as idle sequence state",
            "[gtk][unit][main-window-playback][session]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto trackId = kInvalidTrackId;
    auto fixture = GtkRuntimeFixture{
      [&](library::MusicLibrary& library)
      {
        auto const fixtureUri =
          audio::test::installAudioFixture(library.rootPath(), "basic_metadata.flac", "restored-track.flac");
        trackId = library::test::addTrackWithUniqueFixtureUri(library, {.title = "Restored Track", .uri = fixtureUri});
      }};
    auto& runtime = fixture.runtime();
    rt::test::addReadyAudioProvider(runtime);
    auto& playback = runtime.playback();
    auto const sourceListId = ao::test::requireValue(runGtkTask(runtime,
                                                                runtime.library().commands().createList(rt::ListDraft{
                                                                  .name = "Temporary sequence source",
                                                                })));
    runtime.reloadAllTracks();
    auto const sourceViewId = ao::test::requireValue(runtime.workspace().navigate({.target = sourceListId}));
    REQUIRE(playback.commands().startFromView(sourceViewId, trackId));
    REQUIRE(waitForPlaybackSettlement(runtime, trackId));
    playback.commands().seek(std::chrono::milliseconds{500});
    playback.commands().setShuffleMode(rt::ShuffleMode::On);
    playback.commands().setRepeatMode(rt::RepeatMode::All);
    REQUIRE(runtime.savePlaybackSession());
    REQUIRE(runGtkTask(runtime, runtime.library().commands().deleteList(sourceListId)));
    playback.commands().stop();

    auto const configPath = std::filesystem::path{fixture.tempDir().path()} / "app_config.yaml";
    auto configStorePtr = std::make_shared<AppConfigStore>(configPath);
    auto window = MainWindow{runtime, configStorePtr, nullptr, ao::test::englishMessageCatalog()};

    REQUIRE(window.prepareSession());
    REQUIRE(window.activateSession(MainWindow::PlaybackRestoreMode::Restore));

    // A source list that no longer exists must not strand the restored track:
    // the session comes back on All Tracks, paused where the user left it.
    auto const snapshot = playback.snapshot();
    auto const& sequenceState = snapshot.succession;
    CHECK(sequenceState.currentTrackId == trackId);
    CHECK(sequenceState.sourceListId == rt::kAllTracksListId);
    CHECK(sequenceState.sourceState == rt::PlaybackSourceState::Live);
    CHECK(sequenceState.shuffle == rt::ShuffleMode::On);
    CHECK(sequenceState.repeat == rt::RepeatMode::All);
    CHECK(snapshot.transport.transport == audio::Transport::Idle);
    CHECK(snapshot.transport.nowPlaying.trackId == trackId);
    CHECK(snapshot.transport.elapsed == std::chrono::milliseconds{500});

    auto const focusedViewId = runtime.workspace().snapshot().activeViewId;
    REQUIRE(focusedViewId != rt::kInvalidViewId);
    CHECK(runtime.views().trackListState(focusedViewId).selection == std::vector<TrackId>{trackId});
  }

  TEST_CASE("MainWindow - persists a playback session from playback events",
            "[gtk][unit][main-window-playback][session]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    rt::test::addReadyAudioProvider(runtime);
    auto& playback = runtime.playback();
    auto const fixturePath = audio::test::requireAudioFixture("basic_metadata.flac").string();
    auto const track1 = addRuntimeTrack(runtime, {.title = "Restored Track", .uri = fixturePath});
    auto const track2 = addRuntimeTrack(runtime, {.title = "Changed Track", .uri = fixturePath});

    auto const configPath = std::filesystem::path{fixture.tempDir().path()} / "app_config.yaml";
    auto configStorePtr = std::make_shared<AppConfigStore>(configPath);
    auto window = MainWindow{runtime, configStorePtr, nullptr, ao::test::englishMessageCatalog()};

    REQUIRE(window.prepareSession());
    REQUIRE(window.activateSession(MainWindow::PlaybackRestoreMode::Restore));
    auto const viewId = runtime.workspace().snapshot().activeViewId;
    REQUIRE(viewId != rt::kInvalidViewId);
    auto const* const listOrder = rt::builtinTrackPresentationPreset(rt::kListOrderTrackPresentationId);
    REQUIRE(listOrder != nullptr);
    REQUIRE(runtime.views().setPresentation(viewId, listOrder->spec));
    REQUIRE(playback.commands().startFromView(viewId, track1));
    REQUIRE(waitForPlaybackSettlement(runtime, track1));
    playback.commands().seek(std::chrono::milliseconds{250});
    playback.commands().next();
    playback.commands().seek(std::chrono::milliseconds{550});
    playback.commands().stop();

    auto const restoredRes = runtime.restorePlaybackSession();
    REQUIRE(restoredRes);
    REQUIRE(restoredRes->restored);
    CHECK(restoredRes->trackId == track2);
    auto const snapshot = playback.snapshot();
    CHECK(snapshot.transport.nowPlaying.trackId == track2);
    CHECK(snapshot.transport.elapsed == std::chrono::milliseconds{550});
    CHECK(snapshot.succession.currentTrackId == track2);
    CHECK(snapshot.succession.sourceListId == rt::kAllTracksListId);
    CHECK(snapshot.succession.hasPrevious);
  }
} // namespace ao::gtk::test
