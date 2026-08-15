// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/MainWindow.h"

#include "app/AppConfigStore.h"
#include "app/AppDialog.h"
#include "app/GtkMainContextExecutor.h"
#include "app/LibraryWindowLifecycle.h"
#include "app/WindowState.h"
#include "runtime/PlaybackSessionState.h"
#include "runtime/PlaybackSessionYamlSchema.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include "test/unit/library/MusicLibraryTestSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/linux-gtk/GtkApplicationTestSupport.h"
#include "test/unit/linux-gtk/GtkRuntimeTestSupport.h"
#include "test/unit/runtime/AppRuntimeTestSupport.h"
#include <ao/audio/BackendIds.h>
#include <ao/audio/Device.h>
#include <ao/library/MusicLibrary.h>
#include <ao/rt/AppPrefsState.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ConfigStore.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/uimodel/preference/ThemePreset.h>

#include <catch2/catch_test_macros.hpp>
#include <giomm/actionmap.h>
#include <gtkmm/dialog.h>
#include <gtkmm/window.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <utility>

namespace ao::gtk::test
{
  TEST_CASE("MainWindow - constructs shell with title and window actions", "[gtk][unit][main-window]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};

    auto const configPath = std::filesystem::path{fixture.tempDir().path()} / "app_config.yaml";
    auto configStorePtr = std::make_shared<AppConfigStore>(configPath);
    configStorePtr->saveWindow(WindowState{.width = 640, .height = 480, .maximized = false});

    auto window = MainWindow{fixture.runtime(), configStorePtr, nullptr};

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

    auto window = MainWindow{fixture.runtime(), configStorePtr, nullptr};
    REQUIRE(window.prepareSession());
    REQUIRE(window.activateSession(MainWindow::PlaybackRestoreMode::Restore));

    auto before = rt::AppSessionState{};
    configStorePtr->loadAppSession(before);
    REQUIRE(before.lastLibraryPath.empty());

    window.present();
    window.hide();

    auto after = rt::AppSessionState{};
    configStorePtr->loadAppSession(after);
    CHECK(after.lastLibraryPath == fixture.runtime().musicLibrary().rootPath().string());
  }

  TEST_CASE("MainWindow - explicit session save persists current library path", "[gtk][unit][main-window]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};

    auto const configPath = std::filesystem::path{fixture.tempDir().path()} / "app_config.yaml";
    auto configStorePtr = std::make_shared<AppConfigStore>(configPath);

    auto window = MainWindow{fixture.runtime(), configStorePtr, nullptr};
    REQUIRE(window.prepareSession());
    REQUIRE(window.activateSession(MainWindow::PlaybackRestoreMode::Restore));

    auto before = rt::AppSessionState{};
    configStorePtr->loadAppSession(before);
    REQUIRE(before.lastLibraryPath.empty());

    window.saveSession();

    auto after = rt::AppSessionState{};
    configStorePtr->loadAppSession(after);
    CHECK(after.lastLibraryPath == fixture.runtime().musicLibrary().rootPath().string());
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

    auto window = MainWindow{runtime, configStorePtr, nullptr};
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
      auto window = MainWindow{*runtimePtr, configStorePtr, nullptr};
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
    auto window = MainWindow{*runtimePtr, configStorePtr, nullptr};
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

    auto window = MainWindow{fixture.runtime(), configStorePtr, nullptr};
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
      auto window = MainWindow{fixture.runtime(), configStorePtr, nullptr};
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

    auto windowRes =
      prepareLibraryWindow({.musicRoot = musicRoot, .databasePath = databasePath}, configStorePtr, nullptr, nullptr);
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
} // namespace ao::gtk::test
