// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/MainWindow.h"

#include "app/AppConfigStore.h"
#include "app/AppDialog.h"
#include "app/GtkMainContextExecutor.h"
#include "app/KeymapApplicator.h"
#include "app/LibraryWindowLifecycle.h"
#include "app/ShellLayoutStore.h"
#include "app/WindowState.h"
#include "i18n/GtkText.h"
#include "list/ListNavigationController.h"
#include "playback/OutputDevicePopover.h"
#include "portal/ImportExportCoordinator.h"
#include "portal/LibraryImportExportWorkflow.h"
#include "runtime/PlaybackSessionState.h"
#include "runtime/PlaybackSessionYamlSchema.h"
#include "test/FilesystemTestSupport.h"
#include "test/MessageCatalogTestSupport.h"
#include "test/TestFixtureSupport.h"
#include "test/audio/AudioFixtureSupport.h"
#include "test/library/MusicLibraryTestSupport.h"
#include "test/library/TrackTestSupport.h"
#include "test/linux-gtk/GtkApplicationTestSupport.h"
#include "test/linux-gtk/GtkRuntimeTestSupport.h"
#include "test/linux-gtk/GtkWidgetTestSupport.h"
#include "test/runtime/AppRuntimeTestSupport.h"
#include "track/TrackViewPage.h"
#include <ao/CoreIds.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/Device.h>
#include <ao/audio/OutputDeviceSelection.h>
#include <ao/audio/Transport.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/library/MusicLibrary.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/AppState.h>
#include <ao/rt/ConfigStore.h>
#include <ao/rt/ListMutation.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/PlaybackMode.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryCommands.h>
#include <ao/rt/library/LibrarySnapshot.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/rt/playback/PlaybackSnapshot.h>
#include <ao/rt/source/TrackSourceCache.h>
#include <ao/uimodel/input/KeyChord.h>
#include <ao/uimodel/input/KeymapModel.h>
#include <ao/uimodel/preference/ThemePreset.h>
#include <ao/uimodel/status/activity/ActivityPresentationText.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <giomm/action.h>
#include <giomm/actiongroup.h>
#include <giomm/actionmap.h>
#include <glibmm/ustring.h>
#include <gsl-lite/gsl-lite.hpp>
#include <gtkmm/button.h>
#include <gtkmm/columnview.h>
#include <gtkmm/dialog.h>
#include <gtkmm/listbox.h>
#include <gtkmm/menubutton.h>
#include <gtkmm/popovermenubar.h>
#include <gtkmm/switch.h>
#include <gtkmm/window.h>
#include <sigc++/connection.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace ao::gtk::test
{
  TEST_CASE("MainWindow - restores saved window size and title")
  {
    [[maybe_unused]] auto const appPtr = ensureRegisteredGtkApplication();
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
  }

  TEST_CASE("MainWindow - installs shell and list actions")
  {
    [[maybe_unused]] auto const appPtr = ensureRegisteredGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto configStorePtr = std::make_shared<AppConfigStore>(fixture.tempDir().path() / "app_config.yaml");
    auto window = MainWindow{fixture.runtime(), configStorePtr, nullptr, ao::test::englishMessageCatalog()};

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
    CHECK(actionMap->lookup_action("list-new-playlist") != nullptr);
    CHECK(actionMap->lookup_action("list-edit") != nullptr);
    CHECK(actionMap->lookup_action("list-delete") != nullptr);
    CHECK(actionMap->lookup_action("list-delete-subtree") != nullptr);
  }

  TEST_CASE("MainWindow - scan action stays disabled until its workflow completes")
  {
    [[maybe_unused]] auto const appPtr = ensureRegisteredGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto configStorePtr = std::make_shared<AppConfigStore>(fixture.tempDir().path() / "app_config.yaml");
    auto window = MainWindow{runtime, configStorePtr, nullptr, ao::test::englishMessageCatalog()};
    auto const scanActionPtr = window.lookup_action("scan-library");
    REQUIRE(scanActionPtr);
    REQUIRE(scanActionPtr->get_enabled());

    auto expectedMessage = std::string_view{};
    auto expectedSeverity = rt::NotificationSeverity::Info;
    auto expectedLifetime = rt::NotificationLifetimeKind::Transient;
    bool disabledAtReport = false;
    auto notificationSub = runtime.notifications().onFeedUpdated(
      [&](rt::NotificationFeedUpdate const& update)
      {
        if (update.mutationKind == rt::NotificationFeedMutationKind::Posted)
        {
          auto const& entry = update.feedPtr->entries.back();

          if (auto const* const message = std::get_if<std::string>(&entry.message);
              entry.severity == expectedSeverity && entry.lifetime.kind() == expectedLifetime && message != nullptr &&
              message->contains(expectedMessage))
          {
            disabledAtReport = !scanActionPtr->get_enabled();
          }
        }
      });

    SECTION("bootstrap keeps the action disabled until indexing reports its result")
    {
      expectedMessage = "Audio identity indexing complete";
      audio::test::installAudioFixture(runtime.musicRoot(), "basic_metadata.flac", "song.flac");
      window.importExportCoordinator().scanLibrary(portal::ScanRequestMode::FastBootstrap);
    }

    SECTION("a genuine scan failure is still reported before re-enabling the action")
    {
      expectedMessage = "Library root path does not exist";
      expectedSeverity = rt::NotificationSeverity::Error;
      expectedLifetime = rt::NotificationLifetimeKind::History;
      std::filesystem::rename(runtime.musicRoot(), fixture.tempDir().path() / "unavailable-music");
      scanActionPtr->activate();
    }

    CHECK_FALSE(scanActionPtr->get_enabled());
    REQUIRE(tryPumpGtkEventsUntil([&scanActionPtr] { return scanActionPtr->get_enabled(); }));
    CHECK(disabledAtReport);
  }

  TEST_CASE("MainWindow - destruction unexports window actions before retained handlers can run")
  {
    [[maybe_unused]] auto const appPtr = ensureRegisteredGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto const configPath = std::filesystem::path{fixture.tempDir().path()} / "app_config.yaml";
    auto configStorePtr = std::make_shared<AppConfigStore>(configPath);
    auto removedActionIds = std::vector<std::string>{};
    auto retainedActionPtr = Glib::RefPtr<Gio::Action>{};
    auto actionRemovedConnection = sigc::connection{};

    {
      auto window = MainWindow{fixture.runtime(), configStorePtr, nullptr, ao::test::englishMessageCatalog()};
      auto* const actionGroup = dynamic_cast<Gio::ActionGroup*>(&window);
      REQUIRE(actionGroup != nullptr);
      actionRemovedConnection = actionGroup->signal_action_removed().connect(
        [&removedActionIds](Glib::ustring const& id) { removedActionIds.push_back(id.raw()); });
      retainedActionPtr = window.lookup_action("edit-layout");
      REQUIRE(retainedActionPtr);

      auto const existingWindows = Gtk::Window::list_toplevels();
      retainedActionPtr->activate();
      drainGtkEvents();
      auto const editorWindows = Gtk::Window::list_toplevels();
      REQUIRE(editorWindows.size() == existingWindows.size() + 1);
      auto const editor = std::ranges::find_if(editorWindows,
                                               [&existingWindows](auto* candidate)
                                               { return !std::ranges::contains(existingWindows, candidate); });
      REQUIRE(editor != editorWindows.end());
      CHECK((*editor)->get_transient_for() == &window);
      (*editor)->hide();
      drainGtkEvents();
      REQUIRE(Gtk::Window::list_toplevels().size() == existingWindows.size());
    }

    for (auto const* const expectedId : {"open-library",
                                         "scan-library",
                                         "import-library",
                                         "export-library",
                                         "edit-layout",
                                         "reset-runtime-layout-state",
                                         "save-panel-sizes-as-layout-defaults",
                                         "list-new-smart-list",
                                         "list-new-playlist",
                                         "list-edit",
                                         "list-delete",
                                         "list-delete-subtree"})
    {
      CHECK(std::ranges::contains(removedActionIds, expectedId));
    }

    auto const topLevelCount = Gtk::Window::list_toplevels().size();
    retainedActionPtr->activate();
    drainGtkEvents();
    CHECK(Gtk::Window::list_toplevels().size() == topLevelCount);
  }

  TEST_CASE("MainWindow - hide persists current library path")
  {
    [[maybe_unused]] auto const appPtr = ensureRegisteredGtkApplication();
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

  TEST_CASE("MainWindow - explicit session save persists current library path")
  {
    [[maybe_unused]] auto const appPtr = ensureRegisteredGtkApplication();
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

  TEST_CASE("MainWindow - library switch forgets playback and prevents stale path writes")
  {
    [[maybe_unused]] auto const appPtr = ensureRegisteredGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();

    auto const configPath = std::filesystem::path{fixture.tempDir().path()} / "app_config.yaml";
    auto configStorePtr = std::make_shared<AppConfigStore>(configPath);
    REQUIRE(runtime.workspaceConfigStore().save(
      rt::kPlaybackSessionConfigGroup, rt::PlaybackSessionState{}, rt::PlaybackSessionYamlSchema{}));

    auto window = MainWindow{runtime, configStorePtr, nullptr, ao::test::englishMessageCatalog()};
    REQUIRE(window.prepareSession());
    REQUIRE(window.activateSession(MainWindow::PlaybackRestoreMode::StartIdle));
    REQUIRE(window.retireForLibrarySwitch());
    CHECK(window.sessionPhase() == MainWindow::SessionPhase::Retired);
    CHECK_FALSE(*runtime.workspaceConfigStore().contains(rt::kPlaybackSessionConfigGroup));

    auto switchedSession = rt::AppSessionState{};
    switchedSession.lastLibraryPath = "/tmp/new-library";
    REQUIRE(configStorePtr->saveAppSession(switchedSession));
    window.saveSession();

    auto persistedSession = rt::AppSessionState{};
    configStorePtr->loadAppSession(persistedSession);
    CHECK(persistedSession.lastLibraryPath == "/tmp/new-library");
  }

  TEST_CASE("MainWindow - failed successor root commit isolates root and playback persistence")
  {
    [[maybe_unused]] auto const appPtr = ensureRegisteredGtkApplication();
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

    auto runtimePtr =
      std::make_unique<rt::AppRuntime>(ao::test::requireValue(rt::AppRuntime::create(rt::AppRuntimeDependencies{
        .executorPtr = std::make_unique<GtkMainContextExecutor>(),
        .musicRoot = musicRoot,
        .databasePath = databasePath,
        .musicLibraryPinnedMapBytes = library::test::kTestMusicLibraryMapBytes,
        .workspaceConfigStorePtr = std::make_unique<rt::ConfigStore>(workspacePath),
        .playbackSessionConfigStore = &configStorePtr->playbackSessionStore(),
      })));
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

      // Every shipped GTK preset shows the activity feed, which retains the
      // shared report past the startup scan.
      auto const expected =
        uimodel::librarySwitchNotSavedNotification(ao::test::englishMessageCatalog(), committedRes.error());
      auto const feed = runtimePtr->notifications().feed();
      CHECK(std::ranges::any_of(feed.entries,
                                [&expected](rt::NotificationEntry const& entry)
                                {
                                  return entry.severity == expected.severity && entry.message == expected.message &&
                                         entry.lifetime == expected.lifetime;
                                }));

      REQUIRE(std::filesystem::remove(configDir));
      std::filesystem::rename(parkedConfigDir, configDir);

      auto durableSession = rt::AppSessionState{};
      configStorePtr->loadAppSession(durableSession);
      CHECK(durableSession.lastLibraryPath == oldSession.lastLibraryPath);
      REQUIRE_FALSE(*configStorePtr->playbackSessionStore().contains(rt::kPlaybackSessionConfigGroup));

      SECTION("playback stays blocked while ordinary window state saves")
      {
        REQUIRE(runtimePtr->playback().commands().startFromView(viewId, trackId));
        REQUIRE(tryWaitForPlaybackSettlement(*runtimePtr, trackId));
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

  TEST_CASE("MainWindow - failed retirement reports the error and keeps the active window usable")
  {
    [[maybe_unused]] auto const appPtr = ensureRegisteredGtkApplication();
    auto tempDir = ao::test::TempDir{};
    auto const musicRoot = tempDir.path() / "music";
    auto const databasePath = tempDir.path() / "database";
    auto const invalidPlaybackStorePath = tempDir.path() / "playback-store-directory";
    std::filesystem::create_directories(musicRoot);
    std::filesystem::create_directories(databasePath);
    std::filesystem::create_directories(invalidPlaybackStorePath);
    auto invalidPlaybackStore = rt::ConfigStore{invalidPlaybackStorePath};
    auto runtimePtr =
      std::make_unique<rt::AppRuntime>(ao::test::requireValue(rt::AppRuntime::create(rt::AppRuntimeDependencies{
        .executorPtr = std::make_unique<GtkMainContextExecutor>(),
        .musicRoot = musicRoot,
        .databasePath = databasePath,
        .musicLibraryPinnedMapBytes = library::test::kTestMusicLibraryMapBytes,
        .workspaceConfigStorePtr = std::make_unique<rt::ConfigStore>(tempDir.path() / "workspace.yaml"),
        .playbackSessionConfigStore = &invalidPlaybackStore,
      })));
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
    auto* const errorDialog = findAppDialogByTitle("Unable to Switch Libraries");
    REQUIRE(errorDialog != nullptr);
    CHECK(errorDialog->get_transient_for() == &window);
    CHECK(errorDialog->has_css_class("ao-theme-modern"));
    window.applyTheme(uimodel::ThemePreset::Classic);
    CHECK_FALSE(errorDialog->has_css_class("ao-theme-modern"));
    CHECK(errorDialog->has_css_class("ao-theme-classic"));
    errorDialog->response(Gtk::ResponseType::CLOSE);
    drainGtkEvents();
  }

  TEST_CASE("MainWindow - restores saved output when audio provider is bootstrapped before session load")
  {
    [[maybe_unused]] auto const appPtr = ensureRegisteredGtkApplication();
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

  TEST_CASE("MainWindow - prepared session remains isolated until activation")
  {
    [[maybe_unused]] auto const appPtr = ensureRegisteredGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto const configPath = std::filesystem::path{fixture.tempDir().path()} / "app_config.yaml";
    auto const workspacePath = std::filesystem::path{fixture.tempDir().path()} / "config.yaml";
    auto configStorePtr = std::make_shared<AppConfigStore>(configPath);

    {
      auto window = MainWindow{fixture.runtime(), configStorePtr, nullptr, ao::test::englishMessageCatalog()};
      REQUIRE(window.prepareSession());

      CHECK(window.sessionPhase() == MainWindow::SessionPhase::Prepared);
      CHECK_FALSE(window.hasRequestedMprisStart());
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

  TEST_CASE("MainWindow - application-less activation leaves MPRIS absent")
  {
    [[maybe_unused]] auto const appPtr = ensureRegisteredGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto configStorePtr = std::make_shared<AppConfigStore>(fixture.tempDir().path() / "app_config.yaml");
    auto window = MainWindow{fixture.runtime(), configStorePtr, nullptr, ao::test::englishMessageCatalog()};
    REQUIRE(window.prepareSession());
    REQUIRE_FALSE(window.get_application());

    REQUIRE(window.activateSession(MainWindow::PlaybackRestoreMode::StartIdle));
    CHECK(window.sessionPhase() == MainWindow::SessionPhase::Active);
    CHECK_FALSE(window.hasRequestedMprisStart());
    REQUIRE(window.retireForLibrarySwitch());
    CHECK_FALSE(window.hasRequestedMprisStart());
  }

  TEST_CASE("prepareLibraryWindow - releases C++ and GTK ownership with or without activation")
  {
    auto const appPtr = ensureRegisteredGtkApplication();
    auto tempDir = ao::test::TempDir{};
    auto const musicRoot = tempDir.path() / "music";
    auto const databasePath = tempDir.path() / "database";
    std::filesystem::create_directories(musicRoot);
    auto configStorePtr = std::make_shared<AppConfigStore>(tempDir.path() / "app-config.yaml");
    auto const initialConfigOwners = configStorePtr.use_count();
    auto const weakConfigStorePtr = std::weak_ptr{configStorePtr};
    auto const activate = GENERATE(false, true);

#ifdef AOBUS_HAS_SYSTEM_MEDIA
    if (activate)
    {
      requireOwnedGtkSessionBus();
    }
#endif
    bool finalized = false;

    auto windowRes = prepareLibraryWindow({.musicRoot = musicRoot, .databasePath = databasePath},
                                          configStorePtr,
                                          nullptr,
                                          nullptr,
                                          ao::test::englishMessageCatalog());
    REQUIRE(windowRes);
    auto windowPtr = std::move(*windowRes);
    auto* const windowObject = G_OBJECT(windowPtr->gobj());
    auto const onFinalized = +[](gpointer data, GObject*) { *static_cast<bool*>(data) = true; };
    ::g_object_weak_ref(windowObject, onFinalized, &finalized);
    auto finalizationRegistration = gsl_lite::finally(
      [&]
      {
        if (!finalized)
        {
          ::g_object_weak_unref(windowObject, onFinalized, &finalized);
        }
      });

    CHECK(windowPtr->sessionPhase() == MainWindow::SessionPhase::Prepared);
    CHECK_FALSE(windowPtr->get_application());
    CHECK_FALSE(windowPtr->hasRequestedMprisStart());

    REQUIRE(configStorePtr.use_count() > initialConfigOwners);

    if (activate)
    {
      REQUIRE(activateLibraryWindow(*appPtr, windowPtr, MainWindow::PlaybackRestoreMode::StartIdle));

      CHECK(windowPtr->sessionPhase() == MainWindow::SessionPhase::Active);
      CHECK(windowPtr->get_application() == appPtr);
#ifdef AOBUS_HAS_SYSTEM_MEDIA
      CHECK(windowPtr->hasRequestedMprisStart());
#else
      CHECK_FALSE(windowPtr->hasRequestedMprisStart());
#endif

      REQUIRE(windowPtr->retireForLibrarySwitch());
      windowPtr->close();
      drainGtkEvents();

      if (windowPtr->get_application())
      {
        appPtr->remove_window(*windowPtr);
      }
    }

    windowPtr.reset();
    drainGtkEvents();

    CHECK(finalized);
    // GObject finalization alone does not prove the C++ window and its
    // collaborators released their ownership of application state.
    CHECK(configStorePtr.use_count() == initialConfigOwners);
    configStorePtr.reset();
    CHECK(weakConfigStorePtr.expired());
  }

  TEST_CASE("MainWindow - shell menu components receive the window's menu model")
  {
    [[maybe_unused]] auto const appPtr = ensureRegisteredGtkApplication();
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

    REQUIRE(tryPumpGtkEventsUntil([&findApplicationMenu] { return findApplicationMenu() != nullptr; }));

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

  TEST_CASE("MainWindow - a keymap publishes accelerators only after it is saved")
  {
    // Accelerators publish only to a window added to a started application.
    auto const appPtr = ensureRegisteredGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto const configDir = fixture.tempDir().path() / "global-config";
    auto const configPath = configDir / "config.yaml";
    std::filesystem::create_directories(configDir);
    auto configStorePtr = std::make_shared<AppConfigStore>(configPath);
    auto window = MainWindow{fixture.runtime(), configStorePtr, nullptr, ao::test::englishMessageCatalog()};
    appPtr->add_window(window);

    auto const chord = [](std::string const& text)
    {
      auto const optChord = uimodel::KeyChord::parse(text);
      REQUIRE(optChord);
      return *optChord;
    };
    auto const actionId = std::string{"main-window.keymap-probe"};
    auto const detailedAction = "win." + actionId;
    auto const liveKeymap = uimodel::KeymapModel{uimodel::KeymapBindings{{actionId, {chord("Ctrl+F9")}}}};
    applyKeymapAccelerators(*appPtr, liveKeymap);
    auto const liveAccels = std::vector<Glib::ustring>{"<Control>F9"};
    REQUIRE(appPtr->get_accels_for_action(detailedAction) == liveAccels);

    auto candidate = liveKeymap;
    REQUIRE(candidate.tryBind(actionId, chord("Ctrl+F10")));

    SECTION("a failed save leaves live accelerators unchanged")
    {
      std::filesystem::remove_all(configDir);

      {
        auto blocker = std::ofstream{configDir};
        REQUIRE(blocker);
        blocker << "not a directory";
      }

      CHECK_FALSE(window.applyKeymap(candidate));
      CHECK(appPtr->get_accels_for_action(detailedAction) == liveAccels);
    }

    SECTION("a successful save publishes the saved accelerators")
    {
      REQUIRE(window.applyKeymap(candidate));

      CHECK(appPtr->get_accels_for_action(detailedAction) == std::vector<Glib::ustring>{"<Control>F9", "<Control>F10"});
      auto const reopened = AppConfigStore{configPath}.loadKeymap(liveKeymap.bindings());
      CHECK(reopened.chordsFor(actionId) == candidate.chordsFor(actionId));
    }

    appPtr->set_accels_for_action(detailedAction, {});
    appPtr->remove_window(window);
  }

  TEST_CASE("MainWindow - a session checkpoint does not clobber explicit preferences")
  {
    [[maybe_unused]] auto const appPtr = ensureRegisteredGtkApplication();
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

  TEST_CASE("MainWindow - preparation reports each group a newer version saved and keeps it")
  {
    [[maybe_unused]] auto const appPtr = ensureRegisteredGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto const configPath = std::filesystem::path{fixture.tempDir().path()} / "app_config.yaml";
    std::ofstream{configPath} << "shortcuts:\n  version: 99\n  futureShortcut: kept\n"
                                 "runtime:\n  version: 99\n  futurePreference: kept\n";
    auto configStorePtr = std::make_shared<AppConfigStore>(configPath);

    auto window = MainWindow{runtime, configStorePtr, nullptr, ao::test::englishMessageCatalog()};
    REQUIRE(window.prepareSession());
    REQUIRE(window.activateSession(MainWindow::PlaybackRestoreMode::Restore));
    window.saveSession();

    auto const& catalog = ao::test::englishMessageCatalog();
    auto messages = std::vector<std::string>{};

    for (auto const& entry : runtime.notifications().feed().entries)
    {
      CHECK(entry.severity == rt::NotificationSeverity::Warning);
      REQUIRE(std::holds_alternative<std::string>(entry.message));
      messages.push_back(std::get<std::string>(entry.message));
    }

    auto expected = std::vector{std::string{i18n::requiredText(catalog, i18n::MessageId::ConfigNewerShortcuts)},
                                std::string{i18n::requiredText(catalog, i18n::MessageId::GtkConfigNewerPreferences)}};
    std::ranges::sort(messages);
    std::ranges::sort(expected);
    CHECK(messages == expected);

    // The session checkpoint writes its own groups beside the newer ones.
    auto const saved = ao::test::readFile(configPath);
    CHECK(saved.contains("futureShortcut: kept"));
    CHECK(saved.contains("futurePreference: kept"));
  }

  TEST_CASE("MainWindow - rejected workspace state is not overwritten during preparation")
  {
    [[maybe_unused]] auto const appPtr = ensureRegisteredGtkApplication();
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

  TEST_CASE("MainWindow - partial output preferences fall back to the session output")
  {
    [[maybe_unused]] auto const appPtr = ensureRegisteredGtkApplication();
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

  namespace
  {
    OutputDevicePopover& openOutputPicker(MainWindow& window)
    {
      window.present();
      auto* button = static_cast<Gtk::Button*>(nullptr);
      REQUIRE(tryPumpGtkEventsUntil(
        [&]
        {
          button = findWidgetByClass<Gtk::Button>(window, "ao-output-device-selector-modern");
          return button != nullptr;
        }));
      emitClicked(*button);
      drainGtkEvents();
      auto* const popover = findWidget<OutputDevicePopover>(*button);
      REQUIRE(popover != nullptr);
      return *popover;
    }

    audio::OutputDeviceSelection savedOutputPreference(std::filesystem::path const& configPath)
    {
      auto prefs = rt::AppPrefsState{};
      AppConfigStore{configPath}.loadAppPrefs(prefs);
      return prefs.preferredOutputSelection;
    }
  } // namespace

  TEST_CASE("MainWindow - a disconnected preferred DAC survives restart and never plays through the last output")
  {
    [[maybe_unused]] auto const appPtr = ensureRegisteredGtkApplication();
    auto tempDir = ao::test::TempDir{};
    auto const configPath = tempDir.path() / "app.yaml";
    auto const& catalog = ao::test::englishMessageCatalog();
    auto const dac = audio::OutputDeviceSelection{.backendId = audio::BackendId{"usb"},
                                                  .deviceId = audio::DeviceId{"studio-dac"},
                                                  .profileId = audio::kProfileShared};
    auto const fallback = audio::OutputDeviceSelection{
      .backendId = audio::BackendId{"speakers"}, .deviceId = {}, .profileId = audio::kProfileShared};
    auto const provider = [](audio::OutputDeviceSelection const& selection,
                             std::string name,
                             std::shared_ptr<rt::test::SilentRouteCounts> countsPtr)
    {
      return rt::test::makeReadyAudioProvider(
        {.descriptor = {.id = selection.backendId, .supportedProfiles = {{.id = selection.profileId}}},
         .devices = {{.id = selection.deviceId,
                      .displayName = std::move(name),
                      .isDefault = selection.deviceId.empty(),
                      .backendId = selection.backendId}}},
        std::move(countsPtr));
    };
    // Activate a row in the window's real layout picker; do not write the preference as test setup.
    auto const selectOutput = [](MainWindow& window, std::string const& name)
    {
      auto& popover = openOutputPicker(window);
      auto* const list = findWidget<Gtk::ListBox>(popover);
      REQUIRE(list != nullptr);
      auto const rows = collectAll<Gtk::ListBoxRow>(*list);
      auto const found = std::ranges::find_if(rows, [&](auto* row) { return findLabelByText(*row, name) != nullptr; });
      REQUIRE(found != rows.end());
      emitRowActivated(*list, **found);
      drainGtkEvents();
    };
    auto const preferredOnDisk = [&] { return savedOutputPreference(configPath); };
    auto const offset = std::chrono::milliseconds{250};
    auto track = kInvalidTrackId;
    {
      auto storePtr = std::make_shared<AppConfigStore>(configPath);
      auto prefs = rt::AppPrefsState{};
      prefs.lastLayoutPreset = "modern";
      storePtr->saveAppPrefs(prefs);
      auto runtimePtr =
        rt::test::makeRuntime(tempDir, std::make_unique<GtkMainContextExecutor>(), &storePtr->playbackSessionStore());
      runtimePtr->addAudioProvider(provider(fallback, "Room speakers", {}));
      runtimePtr->addAudioProvider(provider(dac, "Studio DAC", {}));
      track = addRuntimeTrack(
        *runtimePtr,
        {.title = "Saved subject", .uri = audio::test::requireAudioFixture("basic_metadata.flac").string()});
      auto window =
        MainWindow{*runtimePtr, storePtr, std::make_shared<ShellLayoutStore>(tempDir.path() / "layouts"), catalog};
      REQUIRE(window.prepareSession());
      REQUIRE(window.activateSession(MainWindow::PlaybackRestoreMode::Restore));
      selectOutput(window, "Studio DAC");
      REQUIRE(preferredOnDisk() == dac);
      auto& playback = window.playback();
      REQUIRE(playback.snapshot().transport.output.selectedDevice == dac);
      REQUIRE(playback.commands().startFromView(runtimePtr->workspace().snapshot().activeViewId, track));
      REQUIRE(tryWaitForPlaybackSettlement(*runtimePtr, track));
      playback.commands().pause();
      playback.commands().seek(offset);
      window.saveSession();
    }
    drainGtkEvents();

    auto storePtr = std::make_shared<AppConfigStore>(configPath);
    // GTK has a second, independent shutdown record. Make it a viable but unwanted route.
    auto savedSession = rt::AppSessionState{};
    storePtr->loadAppSession(savedSession);
    REQUIRE(savedSession.lastOutputSelection == dac);
    savedSession.lastOutputSelection = fallback;
    REQUIRE(storePtr->saveAppSession(savedSession));
    auto runtimePtr =
      rt::test::makeRuntime(tempDir, std::make_unique<GtkMainContextExecutor>(), &storePtr->playbackSessionStore());
    auto const fallbackCountsPtr = std::make_shared<rt::test::SilentRouteCounts>();
    runtimePtr->addAudioProvider(provider(fallback, "Room speakers", fallbackCountsPtr));
    drainGtkEvents();
    auto window =
      MainWindow{*runtimePtr, storePtr, std::make_shared<ShellLayoutStore>(tempDir.path() / "layouts"), catalog};
    REQUIRE(window.prepareSession());
    REQUIRE(window.activateSession(MainWindow::PlaybackRestoreMode::Restore));
    auto& playback = window.playback();
    REQUIRE(playback.snapshot().transport.nowPlaying.trackId == track);
    REQUIRE(playback.snapshot().transport.elapsed == offset);
    CHECK(playback.snapshot().transport.output.optPendingOutputSelection == dac);
    playback.commands().resume();
    drainGtkEvents();
    CHECK(fallbackCountsPtr->opens.load() == 0);
    CHECK(fallbackCountsPtr->starts.load() == 0);
    REQUIRE(playback.snapshot().transport.transport == audio::Transport::Idle);
    CHECK(playback.snapshot().transport.elapsed == offset);
    window.saveSession();
    CHECK(preferredOnDisk() == dac);

    SECTION("late DAC discovery does not replay the rejected resume")
    {
      auto const dacCountsPtr = std::make_shared<rt::test::SilentRouteCounts>();
      runtimePtr->addAudioProvider(provider(dac, "Studio DAC", dacCountsPtr));
      REQUIRE(tryPumpGtkEventsUntil([&] { return playback.snapshot().transport.output.selectedDevice == dac; }));
      CHECK_FALSE(playback.snapshot().transport.output.optPendingOutputSelection);
      CHECK(dacCountsPtr->opens.load() == 0);
      CHECK(dacCountsPtr->starts.load() == 0);
      playback.commands().resume();
      drainGtkEvents();
      CHECK(dacCountsPtr->starts.load() == 1);
      CHECK(fallbackCountsPtr->opens.load() == 0);
      CHECK(preferredOnDisk() == dac);
    }

    SECTION("explicitly choosing system default permits resume and replaces the saved intent")
    {
      selectOutput(window, "Room speakers");
      REQUIRE(preferredOnDisk() == fallback);
      REQUIRE_FALSE(playback.snapshot().transport.output.optPendingOutputSelection);
      playback.commands().resume();
      drainGtkEvents();
      CHECK(fallbackCountsPtr->starts.load() == 1);
    }

    CHECK(playback.snapshot().transport.transport == audio::Transport::Playing);
    CHECK(playback.snapshot().transport.nowPlaying.trackId == track);
    CHECK(playback.snapshot().transport.elapsed == offset);
  }

  TEST_CASE("MainWindow - PipeWire rerouting consent survives restart and revocation restores silence")
  {
    [[maybe_unused]] auto const appPtr = ensureRegisteredGtkApplication();
    auto tempDir = ao::test::TempDir{};
    auto const configPath = tempDir.path() / "app.yaml";
    // A legacy preference names the disconnected DAC but predates the permission field.
    std::ofstream{configPath} << "runtime:\n"
                                 "  version: 1\n"
                                 "  lastLayoutPreset: modern\n"
                                 "  lastOutputBackendId: pipewire\n"
                                 "  lastOutputDeviceId: node:studio-dac\n"
                                 "  lastOutputProfileId: shared\n";
    auto const fixed = audio::OutputDeviceSelection{.backendId = audio::kBackendPipeWire,
                                                    .deviceId = audio::DeviceId{"node:studio-dac"},
                                                    .profileId = audio::kProfileShared};
    auto allowed = fixed;
    allowed.routingPolicy = audio::OutputRoutingPolicy::AllowSystemReroute;
    auto track = kInvalidTrackId;
    auto const runStartup =
      [&](audio::OutputDeviceSelection const& expected, std::optional<audio::OutputDeviceSelection> const& optNext)
    {
      auto storePtr = std::make_shared<AppConfigStore>(configPath);
      auto runtimePtr =
        rt::test::makeRuntime(tempDir, std::make_unique<GtkMainContextExecutor>(), &storePtr->playbackSessionStore());
      // The only published sink is node:device1, not node:studio-dac. This provider opens only silent backends.
      auto const countsPtr = std::make_shared<rt::test::SilentRouteCounts>();
      runtimePtr->addAudioProvider(rt::test::makeReadyAudioProvider(rt::test::makePipeWireOutputStatus(), countsPtr));

      if (track == kInvalidTrackId)
      {
        track = addRuntimeTrack(
          *runtimePtr,
          {.title = "Reroute consent", .uri = audio::test::requireAudioFixture("basic_metadata.flac").string()});
      }

      drainGtkEvents();
      auto window = MainWindow{*runtimePtr,
                               storePtr,
                               std::make_shared<ShellLayoutStore>(tempDir.path() / "layouts"),
                               ao::test::englishMessageCatalog()};
      REQUIRE(window.prepareSession());
      REQUIRE(window.activateSession(MainWindow::PlaybackRestoreMode::Restore));
      drainGtkEvents();
      auto& playback = window.playback();
      REQUIRE(savedOutputPreference(configPath) == expected);
      REQUIRE(countsPtr->opens.load() == 0);
      auto const startRes = playback.commands().startFromView(runtimePtr->workspace().snapshot().activeViewId, track);

      if (expected == allowed)
      {
        REQUIRE(startRes);
        REQUIRE(
          tryPumpGtkEventsUntil([&] { return playback.snapshot().transport.transport == audio::Transport::Playing; }));
        CHECK(countsPtr->opens.load() == 1);
        CHECK(countsPtr->starts.load() == 1);
        CHECK(playback.snapshot().transport.output.selectedDevice == allowed);
      }
      else
      {
        REQUIRE_FALSE(startRes);
        CHECK(playback.snapshot().transport.output.optPendingOutputSelection == fixed);
        CHECK(playback.snapshot().transport.transport != audio::Transport::Playing);
        CHECK(countsPtr->opens.load() == 0);
        CHECK(countsPtr->starts.load() == 0);
      }

      window.saveSession();
      REQUIRE(savedOutputPreference(configPath) == expected);

      if (optNext)
      {
        auto& popover = openOutputPicker(window);
        auto* const toggle = findWidget<Gtk::Switch>(popover);
        REQUIRE(toggle != nullptr);
        REQUIRE(toggle->get_active() == (expected == allowed));
        REQUIRE(toggle->activate());
        // The switch applies after signal dispatch and may be replaced by its own publication.
        REQUIRE(tryPumpGtkEventsUntil(
          [&]
          {
            auto const& output = playback.snapshot().transport.output;
            return *optNext == allowed ? !output.optPendingOutputSelection && output.selectedDevice == allowed
                                       : output.optPendingOutputSelection == fixed;
          }));
        window.saveSession();
        REQUIRE(savedOutputPreference(configPath) == *optNext);
      }
    };

    runStartup(fixed, allowed);
    runStartup(allowed, fixed);
    runStartup(fixed, std::nullopt);
  }

  TEST_CASE("MainWindow - primary Play requires a selected track unless the current track can resume")
  {
    [[maybe_unused]] auto const appPtr = ensureRegisteredGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    rt::test::addReadyAudioProvider(runtime);
    auto const uri = audio::test::requireAudioFixture("basic_metadata.flac").string();
    auto const browsedTrack = addRuntimeTrack(runtime, {.title = "Browsed track", .uri = uri});
    auto const playingTrack = addRuntimeTrack(runtime, {.title = "Selected track", .uri = uri});
    auto const otherList =
      ao::test::requireValue(runGtkTask(runtime,
                                        runtime.library().commands().createListAsync(
                                          {.name = "Other list", .expression = R"($title = "Browsed track")"})));
    runtime.sources().reloadAllTracks();

    auto storePtr = std::make_shared<AppConfigStore>(fixture.tempDir().path() / "app.yaml");
    auto prefs = rt::AppPrefsState{};
    prefs.lastLayoutPreset = "modern";
    storePtr->saveAppPrefs(prefs);
    auto window = MainWindow{runtime,
                             storePtr,
                             std::make_shared<ShellLayoutStore>(fixture.tempDir().path() / "layouts"),
                             ao::test::englishMessageCatalog()};
    REQUIRE(window.prepareSession());
    REQUIRE(tryPumpGtkEventsUntil(
      [&] { return findWidgetByClass<Gtk::Button>(window, "ao-play-button-prominent") != nullptr; }));
    auto& button = *findWidgetByClass<Gtk::Button>(window, "ao-play-button-prominent");
    auto const actionPtr = window.lookup_action("playback.playPause");
    REQUIRE(actionPtr);
    auto* const page = findWidget<TrackViewPage>(window);
    REQUIRE(page != nullptr);
    REQUIRE(page->projection() != nullptr);
    REQUIRE(page->projection()->size() == 2);
    REQUIRE(page->selectionController().selectedTrackIds().empty());
    auto& playback = runtime.playback();
    REQUIRE(playback.snapshot().transport.ready);
    CHECK_FALSE(button.get_sensitive());
    CHECK_FALSE(actionPtr->get_enabled());
    emitClicked(button);
    drainGtkEvents();
    CHECK(playback.snapshot().transport.nowPlaying.trackId == kInvalidTrackId);

    page->selectionController().selectTrack(playingTrack);
    drainGtkEvents();
    REQUIRE(button.get_sensitive());
    REQUIRE(actionPtr->get_enabled());
    auto const otherView = ao::test::requireValue(window.navigateToList(otherList));
    drainGtkEvents();
    CHECK_FALSE(button.get_sensitive());
    REQUIRE(runtime.workspace().focusView(page->viewId()));
    drainGtkEvents();
    REQUIRE(button.get_sensitive());
    emitClicked(button);
    REQUIRE(tryWaitForPlaybackSettlement(runtime, playingTrack));
    playback.commands().pause();
    auto const offset = std::chrono::milliseconds{250};
    playback.commands().seek(offset);
    REQUIRE(playback.snapshot().transport.transport == audio::Transport::Paused);
    REQUIRE(playback.snapshot().transport.elapsed == offset);

    REQUIRE(runtime.workspace().focusView(otherView));
    REQUIRE(runtime.views().setSelection(otherView, {browsedTrack}));
    drainGtkEvents();
    REQUIRE(button.get_sensitive());
    emitClicked(button);
    drainGtkEvents();
    CHECK(playback.snapshot().transport.transport == audio::Transport::Playing);
    CHECK(playback.snapshot().transport.nowPlaying.trackId == playingTrack);
    CHECK(playback.snapshot().transport.elapsed == offset);
    CHECK(playback.snapshot().succession.sourceListId == rt::kAllTracksListId);
  }

  TEST_CASE("MainWindow - primary Play reports a selected track's synchronous start failure once")
  {
    [[maybe_unused]] auto const appPtr = ensureRegisteredGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    rt::test::addReadyAudioProvider(runtime);
    auto const path = runtime.musicRoot() / "song.flac";
    std::filesystem::copy_file(audio::test::requireAudioFixture("basic_metadata.flac"), path);
    auto const created =
      ao::test::requireValue(runGtkTask(runtime, runtime.library().commands().createTrackFromFileAsync(path)));

    auto storePtr = std::make_shared<AppConfigStore>(fixture.tempDir().path() / "app.yaml");
    auto window = MainWindow{runtime,
                             storePtr,
                             std::make_shared<ShellLayoutStore>(fixture.tempDir().path() / "layouts"),
                             ao::test::englishMessageCatalog()};
    REQUIRE(window.prepareSession());
    REQUIRE(tryPumpGtkEventsUntil([&] { return findWidget<TrackViewPage>(window) != nullptr; }));
    findWidget<TrackViewPage>(window)->selectionController().selectTrack(created.trackId);
    drainGtkEvents();
    auto const actionPtr = window.lookup_action("playback.playPause");
    REQUIRE(actionPtr);
    REQUIRE(actionPtr->get_enabled());
    REQUIRE(runtime.notifications().feed().entries.empty());

    // The row remains selected, but its stored path no longer resolves inside the library.
    auto const relocated = fixture.tempDir().path() / "relocated.flac";
    std::filesystem::rename(path, relocated);
    auto const link = ao::test::SymlinkFixture{relocated, path, ao::test::SymlinkType::File};
    actionPtr->activate();

    auto const feed = runtime.notifications().feed();
    REQUIRE(feed.entries.size() == 1);
    CHECK(feed.entries.front().severity == rt::NotificationSeverity::Error);
    CHECK(feed.entries.front().lifetime == rt::NotificationLifetime::history());
    CHECK(std::get<std::string>(feed.entries.front().message).contains("outside the library root"));
    CHECK(runtime.playback().snapshot().transport.nowPlaying.trackId == kInvalidTrackId);
  }

  TEST_CASE("MainWindow - reveal of a missing playing track reports once without changing focus")
  {
    [[maybe_unused]] auto const appPtr = ensureRegisteredGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto const removedTrack = addRuntimeTrack(runtime, {.title = "Removed track"});
    auto const keptTrack = addRuntimeTrack(runtime, {.title = "Kept track"});
    runtime.sources().reloadAllTracks();

    auto storePtr = std::make_shared<AppConfigStore>(fixture.tempDir().path() / "app.yaml");
    auto prefs = rt::AppPrefsState{};
    prefs.lastLayoutPreset = "modern";
    storePtr->saveAppPrefs(prefs);
    auto window = MainWindow{runtime,
                             storePtr,
                             std::make_shared<ShellLayoutStore>(fixture.tempDir().path() / "layouts"),
                             ao::test::messageCatalog("fr")};
    REQUIRE(window.prepareSession());
    auto& commands = runtime.playback().commands();

    // The label can submit a reveal even when the transport has no subject.
    REQUIRE(runtime.playback().snapshot().transport.nowPlaying.trackId == kInvalidTrackId);
    commands.revealPlayingTrack();
    drainGtkEvents();
    CHECK(runtime.notifications().feed().entries.empty());

    auto const sourceViewId = runtime.workspace().snapshot().activeViewId;
    REQUIRE(sourceViewId != rt::kInvalidViewId);
    {
      auto const library = runtime.library().snapshot();
      auto const optUri = library.trackUri(removedTrack);
      REQUIRE(optUri);
      auto const session = rt::PlaybackSessionState{.libraryId = library.libraryId(),
                                                    .sourceListId = rt::kAllTracksListId,
                                                    .currentTrackId = removedTrack,
                                                    .currentTrackUri = *optUri};
      REQUIRE(
        runtime.workspaceConfigStore().save(rt::kPlaybackSessionConfigGroup, session, rt::PlaybackSessionYamlSchema{}));
    }
    REQUIRE(ao::test::requireValue(runtime.restorePlaybackSession()).restored);
    auto const departureViewId = ao::test::requireValue(runtime.workspace().navigate({
      .target = rt::FilteredListTarget{.listId = rt::kAllTracksListId, .filterExpression = R"($title = "Kept track")"},
    }));
    REQUIRE(runtime.workspace().closeView(sourceViewId));
    REQUIRE(runGtkTask(runtime, runtime.library().commands().deleteTrackAsync(removedTrack)));
    REQUIRE_FALSE(runtime.library().snapshot().containsTrack(removedTrack));
    REQUIRE(runtime.playback().snapshot().transport.nowPlaying.trackId == removedTrack);

    window.present();
    REQUIRE(tryPumpGtkEventsUntil(
      [&]
      {
        auto const* const page = findWidget<TrackViewPage>(window);
        return page != nullptr && page->viewId() == departureViewId && page->get_mapped();
      }));
    auto& page = *findWidget<TrackViewPage>(window);
    page.selectionController().selectTrack(keptTrack);
    auto* const columnView = findWidget<Gtk::ColumnView>(page);
    REQUIRE(columnView != nullptr);
    REQUIRE(columnView->grab_focus());
    drainGtkEvents();
    auto const* const focusedWidget = window.get_focus();
    REQUIRE(focusedWidget != nullptr);
    auto const workspaceBefore = runtime.workspace().snapshot();
    REQUIRE(workspaceBefore.activeViewId == departureViewId);

    // A late explicit page event has no target and must not report a lookup failure.
    commands.revealTrack(removedTrack, sourceViewId, rt::kAllTracksListId);
    drainGtkEvents();
    CHECK(runtime.notifications().feed().entries.empty());

    auto const actionPtr = window.lookup_action(std::string{uimodel::kRevealCurrentTrackActionId});
    REQUIRE(actionPtr);
    REQUIRE(actionPtr->get_enabled());
    actionPtr->activate();
    drainGtkEvents();

    auto const feed = runtime.notifications().feed();
    REQUIRE(feed.entries.size() == 1);
    CHECK(feed.entries.front().severity == rt::NotificationSeverity::Error);
    CHECK(feed.entries.front().lifetime == rt::NotificationLifetime::history());
    CHECK(std::get<std::string>(feed.entries.front().message) ==
          "Échec de navigation : Track is no longer in the library");
    CHECK(runtime.workspace().snapshot() == workspaceBefore);
    CHECK(runtime.views().trackListState(departureViewId).selection == std::vector{keptTrack});
    CHECK(window.get_focus() == focusedWidget);
  }

  TEST_CASE("MainWindow - restores a playback session as idle sequence state")
  {
    [[maybe_unused]] auto const appPtr = ensureRegisteredGtkApplication();
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
    auto const sourceListId =
      ao::test::requireValue(runGtkTask(runtime,
                                        runtime.library().commands().createListAsync(rt::ListDraft{
                                          .name = "Temporary sequence source",
                                        })));
    runtime.sources().reloadAllTracks();
    auto const sourceViewId = ao::test::requireValue(runtime.workspace().navigate({.target = sourceListId}));
    REQUIRE(playback.commands().startFromView(sourceViewId, trackId));
    REQUIRE(tryWaitForPlaybackSettlement(runtime, trackId));
    playback.commands().seek(std::chrono::milliseconds{500});
    playback.commands().setShuffleMode(rt::ShuffleMode::On);
    playback.commands().setRepeatMode(rt::RepeatMode::All);
    REQUIRE(runtime.savePlaybackSession());
    REQUIRE(runGtkTask(runtime, runtime.library().commands().deleteListAsync(sourceListId)));
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

  TEST_CASE("MainWindow - persists a playback session from playback events")
  {
    [[maybe_unused]] auto const appPtr = ensureRegisteredGtkApplication();
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
    REQUIRE(tryWaitForPlaybackSettlement(runtime, track1));
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
