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
#include "test/unit/RuntimeTestSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/linux-gtk/GtkTestSupport.h"
#include <ao/audio/BackendIds.h>
#include <ao/audio/Device.h>
#include <ao/library/MusicLibrary.h>
#include <ao/rt/AppPrefsState.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ConfigStore.h>
#include <ao/uimodel/preference/ThemePreset.h>

#include <catch2/catch_test_macros.hpp>
#include <giomm/actionmap.h>
#include <gtkmm/dialog.h>
#include <gtkmm/window.h>

#include <cstdint>
#include <filesystem>
#include <memory>

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
    REQUIRE(window.activateSession(MainWindow::PlaybackRestoreMode::StartIdle));

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
    REQUIRE(window.activateSession(MainWindow::PlaybackRestoreMode::StartIdle));

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
    configStorePtr->saveAppSession(switchedSession);
    window.saveSession();

    auto persistedSession = rt::AppSessionState{};
    configStorePtr->loadAppSession(persistedSession);
    CHECK(persistedSession.lastLibraryPath == "/tmp/new-library");
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
    auto runtime = rt::AppRuntime{rt::AppRuntimeDependencies{
      .executorPtr = std::make_unique<GtkMainContextExecutor>(),
      .musicRoot = musicRoot,
      .databasePath = databasePath,
      .musicLibraryMapSize = library::test::kTestMusicLibraryMapSize,
      .workspaceConfigStorePtr = std::make_unique<rt::ConfigStore>(tempDir.path() / "workspace.yaml"),
      .playbackSessionConfigStore = &invalidPlaybackStore,
    }};
    auto configStorePtr = std::make_shared<AppConfigStore>(tempDir.path() / "app-config.yaml");
    auto window = MainWindow{runtime, configStorePtr, nullptr};
    REQUIRE(window.prepareSession());
    REQUIRE(window.activateSession(MainWindow::PlaybackRestoreMode::StartIdle));
    window.applyTheme(uimodel::ThemePreset::Modern);

    auto const retired = window.retireForLibrarySwitch();

    REQUIRE_FALSE(retired);
    CHECK(retired.error().code == Error::Code::IoError);
    CHECK(window.sessionPhase() == MainWindow::SessionPhase::Active);
    CHECK(window.musicRoot() == runtime.musicRoot());
    CHECK_NOTHROW(window.playback());

    window.saveSession();
    auto persistedSession = rt::AppSessionState{};
    configStorePtr->loadAppSession(persistedSession);
    CHECK(persistedSession.lastLibraryPath == runtime.musicRoot().string());
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
    prefs.lastOutputBackendId = "test_backend";
    prefs.lastOutputDeviceId = "test_device";
    prefs.lastOutputProfileId = audio::kProfileShared.raw();
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

  TEST_CASE("MainWindow - lifecycle rejects illegal transitions and retirement remains idempotent",
            "[gtk][unit][main-window][session]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto configStorePtr =
      std::make_shared<AppConfigStore>(std::filesystem::path{fixture.tempDir().path()} / "app_config.yaml");
    auto window = MainWindow{fixture.runtime(), configStorePtr, nullptr};

    auto activated = window.activateSession(MainWindow::PlaybackRestoreMode::StartIdle);
    REQUIRE_FALSE(activated);
    CHECK(activated.error().code == Error::Code::InvalidState);

    auto retired = window.retireForLibrarySwitch();
    REQUIRE_FALSE(retired);
    CHECK(retired.error().code == Error::Code::InvalidState);

    REQUIRE(window.prepareSession());
    auto preparedAgain = window.prepareSession();
    REQUIRE_FALSE(preparedAgain);
    CHECK(preparedAgain.error().code == Error::Code::InvalidState);

    REQUIRE(window.activateSession(MainWindow::PlaybackRestoreMode::StartIdle));
    auto activatedAgain = window.activateSession(MainWindow::PlaybackRestoreMode::StartIdle);
    REQUIRE_FALSE(activatedAgain);
    CHECK(activatedAgain.error().code == Error::Code::InvalidState);

    REQUIRE(window.retireForLibrarySwitch());
    REQUIRE(window.retireForLibrarySwitch());
    CHECK(window.sessionPhase() == MainWindow::SessionPhase::Retired);

    auto reactivated = window.activateSession(MainWindow::PlaybackRestoreMode::StartIdle);
    REQUIRE_FALSE(reactivated);
    CHECK(reactivated.error().code == Error::Code::InvalidState);
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

    auto windowPtr =
      prepareLibraryWindow({.musicRoot = musicRoot, .databasePath = databasePath}, configStorePtr, nullptr, nullptr);
    ::g_object_weak_ref(
      G_OBJECT(windowPtr->gobj()), [](gpointer data, GObject*) { *static_cast<bool*>(data) = true; }, &finalized);

    CHECK(windowPtr->sessionPhase() == MainWindow::SessionPhase::Prepared);
    CHECK_FALSE(windowPtr->get_application());
    CHECK_FALSE(windowPtr->isMprisStarted());

    activateLibraryWindow(*appPtr, windowPtr, MainWindow::PlaybackRestoreMode::StartIdle);

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
