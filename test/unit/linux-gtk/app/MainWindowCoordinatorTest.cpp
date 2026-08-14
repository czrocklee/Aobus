// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/MainWindowCoordinator.h"

#include "app/AppConfigStore.h"
#include "app/ThemeCoordinator.h"
#include "portal/ImportExportCoordinator.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/linux-gtk/GtkApplicationTestSupport.h"
#include "test/unit/linux-gtk/GtkRuntimeTestSupport.h"
#include "test/unit/runtime/AppRuntimeTestSupport.h"
#include "track/TrackRowCache.h"
#include "track/TrackRowObject.h"
#include <ao/CoreIds.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/Device.h>
#include <ao/audio/OutputDeviceSelection.h>
#include <ao/audio/Transport.h>
#include <ao/library/MusicLibrary.h>
#include <ao/rt/AppPrefsState.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/PlaybackMode.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryPaths.h>
#include <ao/rt/library/LibraryWriter.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/rt/playback/PlaybackSnapshot.h>
#include <ao/uimodel/library/presentation/ListPresentationPreferenceStore.h>
#include <ao/uimodel/preference/ThemePreset.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/window.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ao::gtk::test
{
  namespace
  {
    TrackId addTrackWithTitle(rt::AppRuntime& runtime, std::string_view title)
    {
      return addRuntimeTrack(runtime, {.title = std::string{title}});
    }

    void updateTrackTitle(rt::AppRuntime& runtime, TrackId trackId, std::string_view title)
    {
      updateRuntimeTrack(runtime, trackId, [&](library::test::TrackSpec& spec) { spec.title = std::string{title}; });
    }
  } // namespace

  TEST_CASE("MainWindowCoordinator - saveSession does not clobber explicit preferences", "[gtk][unit][main-window]")
  {
    auto const appPtr = ensureGtkApplication();
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

    auto window = Gtk::Window{};
    auto coordinator = MainWindowCoordinator{window, runtime, configStorePtr};
    coordinator.loadSession();

    coordinator.themeCoordinator()->setTheme(uimodel::ThemePreset::Classic);
    coordinator.saveSession(MainWindowCoordinator::SessionSavePolicy::Full);

    auto loadedPrefs = rt::AppPrefsState{};
    configStorePtr->loadAppPrefs(loadedPrefs);
    CHECK(loadedPrefs.lastThemePreset == "modern");
    CHECK(loadedPrefs.preferredOutputSelection.backendId == "preference-backend");
    CHECK(loadedPrefs.preferredOutputSelection.deviceId == "preference-device");
    CHECK(loadedPrefs.preferredOutputSelection.profileId == "preference-profile");

    auto loadedSession = rt::AppSessionState{};
    configStorePtr->loadAppSession(loadedSession);
    CHECK(loadedSession.lastLibraryPath == runtime.musicLibrary().rootPath().string());
  }

  TEST_CASE("MainWindowCoordinator - main-window output requests update only the preferred route",
            "[gtk][regression][main-window][audio]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto const configPath = std::filesystem::path{fixture.tempDir().path()} / "app_config.yaml";
    auto configStorePtr = std::make_shared<AppConfigStore>(configPath);
    auto initialPrefs = rt::AppPrefsState{
      .lastLayoutPreset = "classic",
      .lastThemePreset = "modern",
    };
    configStorePtr->saveAppPrefs(initialPrefs);
    auto window = Gtk::Window{};
    auto coordinator = MainWindowCoordinator{window, fixture.runtime(), configStorePtr};
    auto const dependencies = coordinator.uiDependencies();
    auto const selection = audio::OutputDeviceSelection{
      .backendId = audio::BackendId{"pipewire"},
      .deviceId = audio::DeviceId{"headphones"},
      .profileId = audio::kProfileExclusive,
    };

    REQUIRE(dependencies.onOutputDeviceSelectionRequested);
    dependencies.onOutputDeviceSelectionRequested(selection);

    auto loadedPrefs = rt::AppPrefsState{};
    configStorePtr->loadAppPrefs(loadedPrefs);
    CHECK(loadedPrefs.preferredOutputSelection == selection);
    CHECK(loadedPrefs.lastLayoutPreset == "classic");
    CHECK(loadedPrefs.lastThemePreset == "modern");
  }

  TEST_CASE("MainWindowCoordinator - rejected workspace state is not overwritten during initialization",
            "[gtk][unit][main-window][workspace]")
  {
    auto const appPtr = ensureGtkApplication();
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
    auto window = Gtk::Window{};
    auto coordinator = MainWindowCoordinator{window, runtime, configStorePtr};

    coordinator.prepareSession();

    CHECK(ao::test::readFile(workspacePath) == rejected);
    CHECK(runtime.workspace().snapshot().openViews.size() == 1);
    CHECK(runtime.workspace().snapshot().activeViewId != rt::kInvalidViewId);
  }

  TEST_CASE("MainWindowCoordinator - loading a valid layout sibling does not overwrite a rejected group",
            "[gtk][unit][main-window][config]")
  {
    auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto const layoutPath = rt::LibraryPaths{runtime.musicRoot()}.managedDataPath() / "gtk_layout.yaml";
    std::filesystem::create_directories(layoutPath.parent_path());
    auto const stored = std::string{"trackView.columnLayouts:\n"
                                    "  version: 3\n"
                                    "  layouts: []\n"
                                    "trackView.presentations:\n"
                                    "  version: 1\n"
                                    "  preferences:\n"
                                    "    - listId: 42\n"
                                    "      presentationId: albums\n"};
    std::ofstream{layoutPath} << stored;
    auto const appConfigPath = std::filesystem::path{fixture.tempDir().path()} / "app_config.yaml";
    auto configStorePtr = std::make_shared<AppConfigStore>(appConfigPath);
    auto window = Gtk::Window{};
    auto coordinator = MainWindowCoordinator{window, runtime, configStorePtr};

    coordinator.loadSession();

    CHECK(ao::test::readFile(layoutPath) == stored);
    auto const optPresentation = coordinator.trackPresentationPreferences()->presentationIdForList(ListId{42});
    REQUIRE(optPresentation);
    CHECK(*optPresentation == "albums");
  }

  TEST_CASE("MainWindowCoordinator - library changes invalidate cached track rows", "[gtk][unit][main-window]")
  {
    auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto const configPath = std::filesystem::path{fixture.tempDir().path()} / "app_config.yaml";
    auto configStorePtr = std::make_shared<AppConfigStore>(configPath);

    auto window = Gtk::Window{};
    auto coordinator = MainWindowCoordinator{window, runtime, configStorePtr};
    coordinator.prepareSession();

    auto const trackId = addTrackWithTitle(runtime, "Before Import");
    auto const rowBeforePtr = coordinator.trackRowCache()->trackRow(trackId);
    REQUIRE(rowBeforePtr);
    TrackRowObject const& rowBefore = *rowBeforePtr;
    auto const titleBefore = rowBefore.fieldText(rt::TrackField::Title);
    CHECK(titleBefore == "Before Import");

    updateTrackTitle(runtime, trackId, "After Import");

    auto const rowAfterPtr = coordinator.trackRowCache()->trackRow(trackId);
    REQUIRE(rowAfterPtr);
    TrackRowObject const& rowAfter = *rowAfterPtr;
    auto const titleAfter = rowAfter.fieldText(rt::TrackField::Title);
    CHECK(titleAfter == "After Import");
  }

  TEST_CASE("MainWindowCoordinator - partial output preferences fall back to session output",
            "[gtk][unit][main-window]")
  {
    auto const appPtr = ensureGtkApplication();
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

    auto window = Gtk::Window{};
    auto coordinator = MainWindowCoordinator{window, runtime, configStorePtr};
    coordinator.loadSession();
    drainGtkEvents();

    auto const selected = runtime.playback().snapshot().transport.output.selectedDevice;
    CHECK(selected.backendId == audio::BackendId{"test_backend"});
    CHECK(selected.deviceId == audio::DeviceId{"test_device"});
    CHECK(selected.profileId == audio::kProfileShared);
  }

  TEST_CASE("MainWindowCoordinator - restores playback session as idle sequence state",
            "[gtk][unit][main-window-playback][session]")
  {
    auto const appPtr = ensureGtkApplication();
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
    auto const sourceListId = ao::test::requireValue(runtime.library().writer().createList(rt::LibraryWriter::ListDraft{
      .name = "Temporary sequence source",
    }));
    runtime.reloadAllTracks();
    auto const sourceViewId = ao::test::requireValue(runtime.workspace().navigate({.target = sourceListId}));
    REQUIRE(playback.commands().startFromView(sourceViewId, trackId));
    REQUIRE(waitForPlaybackSettlement(runtime, trackId));
    playback.commands().seek(std::chrono::milliseconds{500});
    playback.commands().setShuffleMode(rt::ShuffleMode::On);
    playback.commands().setRepeatMode(rt::RepeatMode::All);
    REQUIRE(runtime.savePlaybackSession());
    REQUIRE(runtime.library().writer().deleteList(sourceListId));
    playback.commands().stop();

    auto const configPath = std::filesystem::path{fixture.tempDir().path()} / "app_config.yaml";
    auto configStorePtr = std::make_shared<AppConfigStore>(configPath);
    auto window = Gtk::Window{};
    auto coordinator = MainWindowCoordinator{window, runtime, configStorePtr};

    coordinator.prepareSession();
    coordinator.restorePlaybackSession();

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

  TEST_CASE("MainWindowCoordinator - persists playback session from playback events",
            "[gtk][unit][main-window-playback][session]")
  {
    auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    rt::test::addReadyAudioProvider(runtime);
    auto& playback = runtime.playback();
    auto const fixturePath = audio::test::requireAudioFixture("basic_metadata.flac").string();
    auto const track1 = addRuntimeTrack(runtime, {.title = "Restored Track", .uri = fixturePath});
    auto const track2 = addRuntimeTrack(runtime, {.title = "Changed Track", .uri = fixturePath});

    auto const configPath = std::filesystem::path{fixture.tempDir().path()} / "app_config.yaml";
    auto configStorePtr = std::make_shared<AppConfigStore>(configPath);
    auto window = Gtk::Window{};
    auto coordinator = MainWindowCoordinator{window, runtime, configStorePtr};

    coordinator.prepareSession();
    coordinator.restorePlaybackSession();
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
