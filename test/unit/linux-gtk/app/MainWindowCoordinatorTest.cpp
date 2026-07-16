// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/MainWindowCoordinator.h"

#include "../GtkTestSupport.h"
#include "app/AppConfigStore.h"
#include "app/MainWindow.h"
#include "app/ThemeCoordinator.h"
#include "portal/ImportExportCoordinator.h"
#include "test/unit/RuntimeTestSupport.h"
#include "test/unit/TestUtils.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "track/TrackRowCache.h"
#include "track/TrackRowObject.h"
#include <ao/CoreIds.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/Device.h>
#include <ao/audio/Transport.h>
#include <ao/library/MusicLibrary.h>
#include <ao/rt/AppPrefsState.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/PlaybackMode.h>
#include <ao/rt/PlaybackSequenceService.h>
#include <ao/rt/PlaybackState.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryWriter.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <memory>
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
    initialPrefs.lastOutputBackendId = "preference-backend";
    initialPrefs.lastOutputDeviceId = "preference-device";
    initialPrefs.lastOutputProfileId = "preference-profile";
    configStorePtr->saveAppPrefs(initialPrefs);

    auto window = MainWindow{runtime, configStorePtr, nullptr};
    auto coordinator = MainWindowCoordinator{window, runtime, configStorePtr};
    coordinator.loadSession();

    coordinator.themeCoordinator()->setTheme(rt::ThemePresetId::Classic);
    coordinator.saveSession();

    auto loadedPrefs = rt::AppPrefsState{};
    configStorePtr->loadAppPrefs(loadedPrefs);
    CHECK(loadedPrefs.lastThemePreset == "modern");
    CHECK(loadedPrefs.lastOutputBackendId == "preference-backend");
    CHECK(loadedPrefs.lastOutputDeviceId == "preference-device");
    CHECK(loadedPrefs.lastOutputProfileId == "preference-profile");

    auto loadedSession = rt::AppSessionState{};
    configStorePtr->loadAppSession(loadedSession);
    CHECK(loadedSession.lastLibraryPath == runtime.musicLibrary().rootPath().string());
  }

  TEST_CASE("MainWindowCoordinator - import mutation refreshes cached track rows", "[gtk][unit][main-window]")
  {
    auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto const configPath = std::filesystem::path{fixture.tempDir().path()} / "app_config.yaml";
    auto configStorePtr = std::make_shared<AppConfigStore>(configPath);

    auto window = MainWindow{runtime, configStorePtr, nullptr};
    auto coordinator = MainWindowCoordinator{window, runtime, configStorePtr};

    auto const trackId = addTrackWithTitle(runtime, "Before Import");
    auto const rowBeforePtr = coordinator.trackRowCache()->trackRow(trackId);
    REQUIRE(rowBeforePtr);
    CHECK(rowBeforePtr->fieldText(rt::TrackField::Title) == "Before Import");

    updateTrackTitle(runtime, trackId, "After Import");
    CHECK(coordinator.trackRowCache()->trackRow(trackId)->fieldText(rt::TrackField::Title) == "Before Import");

    coordinator.importExport().callbacks().onLibraryDataMutated();

    auto const rowAfterPtr = coordinator.trackRowCache()->trackRow(trackId);
    REQUIRE(rowAfterPtr);
    CHECK(rowAfterPtr->fieldText(rt::TrackField::Title) == "After Import");
  }

  TEST_CASE("MainWindowCoordinator - partial output preferences fall back to session output",
            "[gtk][unit][main-window]")
  {
    auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    rt::test::addReadyAudioProvider(runtime.playback());

    auto const configPath = std::filesystem::path{fixture.tempDir().path()} / "app_config.yaml";
    auto configStorePtr = std::make_shared<AppConfigStore>(configPath);

    auto prefs = rt::AppPrefsState{};
    prefs.lastOutputBackendId = "incomplete-preference-backend";
    prefs.lastOutputProfileId = {};
    configStorePtr->saveAppPrefs(prefs);

    auto session = rt::AppSessionState{};
    session.lastOutputBackendId = "test_backend";
    session.lastOutputDeviceId = "test_device";
    session.lastOutputProfileId = audio::kProfileShared.raw();
    configStorePtr->saveAppSession(session);

    auto window = MainWindow{runtime, configStorePtr, nullptr};
    auto coordinator = MainWindowCoordinator{window, runtime, configStorePtr};
    coordinator.loadSession();

    auto const& selected = runtime.playback().state().output.selectedDevice;
    CHECK(selected.backendId == audio::BackendId{"test_backend"});
    CHECK(selected.deviceId == audio::DeviceId{"test_device"});
    CHECK(selected.profileId == audio::kProfileShared);
  }

  TEST_CASE("MainWindowCoordinator - restores playback session as idle sequence state",
            "[gtk][unit][main-window-playback][session]")
  {
    auto const appPtr = ensureGtkApplication();
    auto const fixturePath = audio::test::requireAudioFixture("basic_metadata.flac").string();
    auto trackId = kInvalidTrackId;
    auto fixture = GtkRuntimeFixture{
      [&](library::MusicLibrary& library)
      { trackId = library::test::addTrack(library, {.title = "Restored Track", .uri = fixturePath}); }};
    auto& runtime = fixture.runtime();
    rt::test::addReadyAudioProvider(runtime.playback());
    auto const sourceListId = ao::test::requireValue(runtime.library().writer().createList(rt::LibraryWriter::ListDraft{
      .kind = rt::LibraryWriter::ListKind::Manual,
      .name = "Temporary sequence source",
      .trackIds = {trackId},
    }));
    runtime.reloadAllTracks();
    auto const sourceViewId = ao::test::requireValue(runtime.workspace().navigateTo(sourceListId)).activeViewId;
    REQUIRE(runtime.playbackSequence().playFromView(sourceViewId, trackId));
    runtime.playback().seek(std::chrono::milliseconds{500});
    runtime.playbackSequence().setShuffleMode(rt::ShuffleMode::On);
    runtime.playbackSequence().setRepeatMode(rt::RepeatMode::All);
    REQUIRE(runtime.savePlaybackSession());
    REQUIRE(runtime.library().writer().deleteList(sourceListId));
    runtime.playback().stop();

    auto const configPath = std::filesystem::path{fixture.tempDir().path()} / "app_config.yaml";
    auto configStorePtr = std::make_shared<AppConfigStore>(configPath);
    auto window = MainWindow{runtime, configStorePtr, nullptr};
    auto coordinator = MainWindowCoordinator{window, runtime, configStorePtr};

    coordinator.initializeSession();

    CHECK(coordinator.playbackSequence() == &runtime.playbackSequence());
    auto const& sequenceState = runtime.playbackSequence().state();
    CHECK(sequenceState.currentTrackId == trackId);
    CHECK(sequenceState.sourceListId == rt::kAllTracksListId);
    CHECK(sequenceState.sourceState == rt::PlaybackSequenceSourceState::Live);
    CHECK(sequenceState.shuffle == rt::ShuffleMode::On);
    CHECK(sequenceState.repeat == rt::RepeatMode::All);
    CHECK(runtime.playback().state().transport == audio::Transport::Idle);
    CHECK(runtime.playback().state().nowPlaying.trackId == trackId);
    CHECK(runtime.playback().state().elapsed == std::chrono::milliseconds{500});

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
    rt::test::addReadyAudioProvider(runtime.playback());
    auto const fixturePath = audio::test::requireAudioFixture("basic_metadata.flac").string();
    auto const track1 = addRuntimeTrack(runtime, {.title = "Restored Track", .uri = fixturePath});
    auto const track2 = addRuntimeTrack(runtime, {.title = "Changed Track", .uri = fixturePath});

    auto const configPath = std::filesystem::path{fixture.tempDir().path()} / "app_config.yaml";
    auto configStorePtr = std::make_shared<AppConfigStore>(configPath);
    auto window = MainWindow{runtime, configStorePtr, nullptr};
    auto coordinator = MainWindowCoordinator{window, runtime, configStorePtr};

    coordinator.initializeSession();
    auto const viewId = runtime.workspace().snapshot().activeViewId;
    REQUIRE(viewId != rt::kInvalidViewId);
    auto const* const listOrder = rt::builtinTrackPresentationPreset(rt::kListOrderTrackPresentationId);
    REQUIRE(listOrder != nullptr);
    REQUIRE(runtime.views().setPresentation(viewId, listOrder->spec));
    REQUIRE(runtime.playbackSequence().playFromView(viewId, track1));
    runtime.playback().seek(std::chrono::milliseconds{250});
    runtime.playbackSequence().next();
    runtime.playback().seek(std::chrono::milliseconds{550});
    runtime.playback().stop();

    auto const restored = runtime.restorePlaybackSession();
    REQUIRE(restored);
    REQUIRE(restored->restored);
    CHECK(restored->trackId == track2);
    CHECK(runtime.playback().state().nowPlaying.trackId == track2);
    CHECK(runtime.playback().state().elapsed == std::chrono::milliseconds{550});
    CHECK(runtime.playbackSequence().state().currentTrackId == track2);
    CHECK(runtime.playbackSequence().state().sourceListId == rt::kAllTracksListId);
    CHECK(runtime.playbackSequence().state().hasPrevious);
  }
} // namespace ao::gtk::test
