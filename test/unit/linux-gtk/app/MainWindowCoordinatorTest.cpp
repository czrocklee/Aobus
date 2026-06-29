// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/MainWindowCoordinator.h"

#include "../GtkTestSupport.h"
#include "app/AppConfig.h"
#include "app/MainWindow.h"
#include "app/ThemeCoordinator.h"
#include "portal/ImportExportCoordinator.h"
#include "test/unit/TestUtils.h"
#include "track/TrackRowCache.h"
#include "track/TrackRowObject.h"
#include <ao/AudioScalars.h>
#include <ao/CoreIds.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackStore.h>
#include <ao/rt/AppPrefsState.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/TrackField.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <memory>
#include <string_view>

namespace ao::gtk::test
{
  namespace
  {
    TrackId addTrackWithTitle(rt::AppRuntime& runtime, std::string_view title)
    {
      auto& library = runtime.musicLibrary();
      auto txn = library.writeTransaction();
      auto writer = library.tracks().writer(txn);

      auto builder = library::TrackBuilder::createNew();
      builder.metadata().title(title).artist("Artist");
      builder.property()
        .uri("/tmp/main-window-coordinator-test.flac")
        .duration(std::chrono::minutes{3})
        .sampleRate(SampleRate{44100})
        .channels(Channels{2})
        .bitDepth(BitDepth{16});

      auto serialized = ao::test::requireValue(builder.serialize(txn, library.dictionary(), library.resources()));
      auto const [trackId, _] = ao::test::requireValue(writer.createHotCold(serialized.first, serialized.second));
      REQUIRE(txn.commit());
      return trackId;
    }

    void updateTrackTitle(rt::AppRuntime& runtime, TrackId trackId, std::string_view title)
    {
      auto& library = runtime.musicLibrary();
      auto txn = library.writeTransaction();
      auto writer = library.tracks().writer(txn);

      auto const optView = writer.get(trackId, library::TrackStore::Reader::LoadMode::Both);
      REQUIRE(optView.has_value());

      auto builder = library::TrackBuilder::fromView(*optView, library.dictionary());
      builder.metadata().title(title);
      auto hotData = ao::test::requireValue(builder.serializeHot(txn, library.dictionary()));
      REQUIRE(writer.updateHot(trackId, hotData));
      REQUIRE(txn.commit());
    }
  } // namespace

  TEST_CASE("MainWindowCoordinator - saveSession preserves selected theme", "[gtk][unit][main-window]")
  {
    auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto const configPath = std::filesystem::path{fixture.tempDir().path()} / "app_config.yaml";
    auto configPtr = std::make_shared<AppConfig>(configPath);

    // Pre-populate config with some playback/library prefs
    auto initialPrefs = rt::AppPrefsState{};
    initialPrefs.lastLibraryPath = "/initial/path";
    initialPrefs.lastThemePreset = "classic";
    configPtr->saveAppPrefs(initialPrefs);

    auto window = MainWindow{runtime, configPtr, nullptr};
    auto coordinator = MainWindowCoordinator{window, runtime, configPtr};
    coordinator.loadSession();

    // Set theme to modern
    coordinator.themeController()->setTheme(rt::ThemePresetId::Modern);

    // Save session
    coordinator.saveSession();

    // Reload and verify
    auto loaded = rt::AppPrefsState{};
    configPtr->loadAppPrefs(loaded);

    CHECK(loaded.lastThemePreset == "modern");
    CHECK(loaded.lastLibraryPath == runtime.musicLibrary().rootPath().string());
  }

  TEST_CASE("MainWindowCoordinator - import mutation refreshes cached track rows", "[gtk][unit][main-window]")
  {
    auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto const configPath = std::filesystem::path{fixture.tempDir().path()} / "app_config.yaml";
    auto configPtr = std::make_shared<AppConfig>(configPath);

    auto window = MainWindow{runtime, configPtr, nullptr};
    auto coordinator = MainWindowCoordinator{window, runtime, configPtr};

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
} // namespace ao::gtk::test
