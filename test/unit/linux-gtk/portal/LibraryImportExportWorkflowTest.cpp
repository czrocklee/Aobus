// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "portal/LibraryImportExportWorkflow.h"

#include "portal/ImportExportCallbacks.h"
#include "test/unit/MessageCatalogTestSupport.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include "test/unit/library/MusicLibraryTestSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/linux-gtk/GtkApplicationTestSupport.h"
#include "test/unit/linux-gtk/GtkRuntimeTestSupport.h"
#include "test/unit/runtime/ExecutorTestSupport.h"
#include "test/unit/runtime/RuntimeLibraryTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ConfigStore.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryJobs.h>
#include <ao/rt/library/LibrarySnapshot.h>
#include <ao/rt/library/LibraryTaskEvents.h>
#include <ao/rt/library/LibraryYamlExporter.h>
#include <ao/rt/source/TrackSourceCache.h>

#include <catch2/catch_test_macros.hpp>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <ios>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::gtk::test
{
  namespace
  {
    bool hasNotification(GtkRuntimeFixture& fixture, rt::NotificationSeverity severity, std::string_view message)
    {
      auto const feed = fixture.runtime().notifications().feed();

      return std::ranges::any_of(
        feed.entries,
        [&](auto const& entry)
        { return entry.severity == severity && std::get<std::string>(entry.message) == message; });
    }

    bool hasNotificationContaining(GtkRuntimeFixture& fixture,
                                   rt::NotificationSeverity severity,
                                   std::string_view messageFragment)
    {
      auto const feed = fixture.runtime().notifications().feed();

      return std::ranges::any_of(
        feed.entries,
        [&](auto const& entry)
        { return entry.severity == severity && std::get<std::string>(entry.message).contains(messageFragment); });
    }

    portal::ImportExportCallbacks confirmingCallbacks()
    {
      return portal::ImportExportCallbacks{
        .requestLibraryRestoreConfirmation = [](rt::ImportReport const&, std::function<void(bool)> completion)
        { completion(true); },
      };
    }

    std::vector<std::string> trackTitles(GtkRuntimeFixture& fixture)
    {
      auto titles = std::vector<std::string>{};

      for (auto const trackId : rt::test::runtimeTrackIds(fixture.runtime()))
      {
        titles.push_back(rt::test::runtimeTrackSpec(fixture.runtime(), trackId).title);
      }

      return titles;
    }

    bool libraryHasTrackTitle(GtkRuntimeFixture& fixture, std::string_view expectedTitle)
    {
      auto const titles = trackTitles(fixture);
      return std::ranges::any_of(titles, [&](std::string const& title) { return title == expectedTitle; });
    }

    std::vector<std::string> trackUris(GtkRuntimeFixture& fixture)
    {
      auto uris = std::vector<std::string>{};
      auto snapshot = fixture.runtime().library().snapshot();

      for (auto const trackId : rt::test::runtimeTrackIds(fixture.runtime()))
      {
        auto const optRow = snapshot.trackRow(trackId);
        REQUIRE(optRow);
        REQUIRE(optRow->optUriPath);
        uris.push_back(optRow->optUriPath->lexically_relative(fixture.runtime().musicRoot()).generic_string());
      }

      return uris;
    }

    void copyMetadataFixtureToLibrary(GtkRuntimeFixture& fixture)
    {
      auto const sourceFile = audio::test::requireAudioFixture("basic_metadata.flac");
      std::filesystem::copy_file(sourceFile, fixture.runtime().musicRoot() / "song.flac");
    }

    std::string readTextFile(std::filesystem::path const& path)
    {
      auto input = std::ifstream{path};
      return std::string{std::istreambuf_iterator{input}, std::istreambuf_iterator<char>{}};
    }
  } // namespace

  TEST_CASE("LibraryImportExportWorkflow - scan reports up-to-date empty library", "[gtk][unit][workflow][scan]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto callbacks = confirmingCallbacks();
    auto workflow =
      portal::LibraryImportExportWorkflow{fixture.runtime(), callbacks, ao::test::englishMessageCatalog()};

    std::int32_t progressFinishedCount = 0;
    auto progressFinishedSub = fixture.runtime().library().jobs().onProgressFinished(
      [&progressFinishedCount](rt::LibraryTaskProgressFinished const&) noexcept { ++progressFinishedCount; });

    workflow.scan();

    REQUIRE(pumpGtkEventsUntil(
      [&fixture, &progressFinishedCount]
      { return progressFinishedCount == 1 && !fixture.runtime().notifications().feed().entries.empty(); }));

    CHECK(progressFinishedCount == 1);
    auto const feed = fixture.runtime().notifications().feed();
    REQUIRE(feed.entries.size() == 1);
    CHECK(feed.entries.back().severity == rt::NotificationSeverity::Info);
    CHECK(std::get<std::string>(feed.entries.back().message) == "Library is up to date");
  }

  TEST_CASE("LibraryImportExportWorkflow - scan mutates only when files change", "[gtk][unit][workflow][scan]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto callbacks = confirmingCallbacks();
    auto workflow =
      portal::LibraryImportExportWorkflow{fixture.runtime(), callbacks, ao::test::englishMessageCatalog()};

    copyMetadataFixtureToLibrary(fixture);

    std::int32_t progressFinishedCount = 0;
    auto progressEvents = std::vector<rt::LibraryTaskProgressUpdated>{};
    auto progressFinishedSub = fixture.runtime().library().jobs().onProgressFinished(
      [&progressFinishedCount](rt::LibraryTaskProgressFinished const&) noexcept { ++progressFinishedCount; });
    auto progressSub = fixture.runtime().library().jobs().onProgress([&progressEvents](auto const& event) noexcept
                                                                     { progressEvents.push_back(event); });

    workflow.scan();
    REQUIRE(pumpGtkEventsUntil(
      [&fixture, &progressFinishedCount]
      {
        return progressFinishedCount == 2 &&
               hasNotification(fixture, rt::NotificationSeverity::Info, "Library scan complete");
      }));
    CHECK(progressFinishedCount == 2);
    REQUIRE(progressEvents.size() == 3);
    CHECK(progressEvents[0].kind == rt::LibraryTaskProgressKind::Scanning);
    CHECK(progressEvents[0].subject == "song.flac");
    CHECK(progressEvents[0].fraction == 0.0);
    CHECK(progressEvents[1].kind == rt::LibraryTaskProgressKind::Updating);
    CHECK(progressEvents[1].subject == "song.flac");
    CHECK(progressEvents[1].fraction == 0.0);
    CHECK(progressEvents[2].kind == rt::LibraryTaskProgressKind::Fingerprinting);
    CHECK(progressEvents[2].subject == "song.flac");
    CHECK(progressEvents[2].fraction == 1.0);
    CHECK(trackTitles(fixture) == std::vector<std::string>{"Test Title"});

    progressFinishedCount = 0;
    progressEvents.clear();

    workflow.scan();

    REQUIRE(pumpGtkEventsUntil(
      [&fixture, &progressFinishedCount, &progressEvents]
      {
        return progressEvents.size() == 1 && progressFinishedCount == 1 &&
               hasNotification(fixture, rt::NotificationSeverity::Info, "Library is up to date");
      }));

    CHECK(progressFinishedCount == 1);
    REQUIRE(progressEvents.size() == 1);
    CHECK(progressEvents[0].kind == rt::LibraryTaskProgressKind::Scanning);
    CHECK(progressEvents[0].subject == "song.flac");
    CHECK(progressEvents[0].fraction == 0.0);
  }

  TEST_CASE("LibraryImportExportWorkflow - fast bootstrap background identity relinks a moved file",
            "[gtk][unit][workflow][scan]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto callbacks = confirmingCallbacks();
    auto workflow =
      portal::LibraryImportExportWorkflow{fixture.runtime(), callbacks, ao::test::englishMessageCatalog()};

    copyMetadataFixtureToLibrary(fixture);

    workflow.scan(portal::ScanRequestMode::FastBootstrap);

    REQUIRE(pumpGtkEventsUntil(
      [&fixture]
      { return hasNotification(fixture, rt::NotificationSeverity::Info, "Audio identity indexing complete"); }));

    CHECK(
      hasNotification(fixture, rt::NotificationSeverity::Info, "Library ready; indexing audio identity in background"));
    CHECK(trackUris(fixture) == std::vector<std::string>{"song.flac"});

    std::filesystem::rename(
      fixture.runtime().musicRoot() / "song.flac", fixture.runtime().musicRoot() / "renamed.flac");
    workflow.scan();

    REQUIRE(pumpGtkEventsUntil(
      [&fixture] { return hasNotification(fixture, rt::NotificationSeverity::Info, "Relinked 1 moved file"); }));
    CHECK(trackUris(fixture) == std::vector<std::string>{"renamed.flac"});
  }

  TEST_CASE("LibraryImportExportWorkflow - scan reports relinked moved files", "[gtk][unit][workflow][scan]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto callbacks = confirmingCallbacks();
    auto workflow =
      portal::LibraryImportExportWorkflow{fixture.runtime(), callbacks, ao::test::englishMessageCatalog()};

    copyMetadataFixtureToLibrary(fixture);
    workflow.scan();
    REQUIRE(pumpGtkEventsUntil(
      [&fixture] { return hasNotification(fixture, rt::NotificationSeverity::Info, "Library scan complete"); }));

    auto const movedPath = fixture.runtime().musicRoot() / "renamed.flac";
    std::filesystem::rename(fixture.runtime().musicRoot() / "song.flac", movedPath);

    workflow.scan();

    REQUIRE(pumpGtkEventsUntil(
      [&fixture] { return hasNotification(fixture, rt::NotificationSeverity::Info, "Relinked 1 moved file"); }));

    CHECK(trackUris(fixture) == std::vector<std::string>{"renamed.flac"});
  }

  TEST_CASE("LibraryImportExportWorkflow - scan reports missing files needing review", "[gtk][unit][workflow][scan]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto callbacks = confirmingCallbacks();
    auto workflow =
      portal::LibraryImportExportWorkflow{fixture.runtime(), callbacks, ao::test::englishMessageCatalog()};

    copyMetadataFixtureToLibrary(fixture);
    workflow.scan();
    REQUIRE(pumpGtkEventsUntil(
      [&fixture] { return hasNotification(fixture, rt::NotificationSeverity::Info, "Library scan complete"); }));

    std::filesystem::remove(fixture.runtime().musicRoot() / "song.flac");

    workflow.scan();

    REQUIRE(pumpGtkEventsUntil(
      [&fixture]
      { return hasNotification(fixture, rt::NotificationSeverity::Warning, "1 missing file needs review"); }));
  }

  TEST_CASE("LibraryImportExportWorkflow - scan reports missing files even when errors occur",
            "[gtk][unit][workflow][scan]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto callbacks = confirmingCallbacks();
    auto workflow =
      portal::LibraryImportExportWorkflow{fixture.runtime(), callbacks, ao::test::englishMessageCatalog()};

    copyMetadataFixtureToLibrary(fixture);
    workflow.scan();
    REQUIRE(pumpGtkEventsUntil(
      [&fixture] { return hasNotification(fixture, rt::NotificationSeverity::Info, "Library scan complete"); }));

    std::filesystem::remove(fixture.runtime().musicRoot() / "song.flac");
    {
      auto out = std::ofstream{fixture.runtime().musicRoot() / "corrupted.flac", std::ios::binary};
      out << "NOT A FLAC FILE";
    }

    workflow.scan();

    REQUIRE(pumpGtkEventsUntil(
      [&fixture]
      {
        return hasNotification(
          fixture, rt::NotificationSeverity::Warning, "Scan completed with errors; 1 missing file needs review");
      }));
  }

  TEST_CASE("LibraryImportExportWorkflow - scan reports error-only plans without up-to-date success",
            "[gtk][unit][workflow][scan]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto callbacks = confirmingCallbacks();
    auto workflow =
      portal::LibraryImportExportWorkflow{fixture.runtime(), callbacks, ao::test::englishMessageCatalog()};

    auto const restrictedDir = fixture.runtime().musicRoot() / "restricted_dir";
    std::filesystem::create_directories(restrictedDir);
    std::filesystem::permissions(restrictedDir, std::filesystem::perms::none);

    if (::geteuid() == 0)
    {
      SKIP("permissions test is meaningless when running as root");
    }

    workflow.scan();

    // The count is what makes this actionable: "scan failed" alone leaves the
    // reader no way to tell one bad file from a whole unreadable library.
    REQUIRE(pumpGtkEventsUntil(
      [&fixture]
      {
        return hasNotification(
          fixture, rt::NotificationSeverity::Error, "Library scan found 1 unreadable file and no usable changes");
      }));

    std::filesystem::permissions(restrictedDir, std::filesystem::perms::owner_all);

    CHECK_FALSE(hasNotification(fixture, rt::NotificationSeverity::Info, "Library is up to date"));
  }

  TEST_CASE("LibraryImportExportWorkflow - export writes scanned track metadata to YAML", "[gtk][unit][workflow][yaml]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto callbacks = confirmingCallbacks();
    auto workflow =
      portal::LibraryImportExportWorkflow{fixture.runtime(), callbacks, ao::test::englishMessageCatalog()};
    auto const target = fixture.tempDir().path() / "library_backup.yaml";

    copyMetadataFixtureToLibrary(fixture);
    workflow.scan();
    REQUIRE(pumpGtkEventsUntil(
      [&fixture] { return hasNotification(fixture, rt::NotificationSeverity::Info, "Library scan complete"); }));
    REQUIRE(libraryHasTrackTitle(fixture, "Test Title"));
    workflow.exportTo(target, rt::ExportMode::Full);

    REQUIRE(pumpGtkEventsUntil(
      [&fixture, &target]
      {
        return std::filesystem::exists(target) &&
               hasNotification(fixture, rt::NotificationSeverity::Info, "Library exported successfully");
      }));

    auto const exportedYaml = readTextFile(target);
    CHECK(std::string_view{exportedYaml}.contains("Test Title"));
    CHECK(std::string_view{exportedYaml}.contains("Test Artist"));
  }

  TEST_CASE("LibraryImportExportWorkflow - import restores track metadata through runtime changes",
            "[gtk][unit][workflow][yaml]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto sourceFixture = GtkRuntimeFixture{};
    auto targetFixture = GtkRuntimeFixture{};
    auto callbacks = confirmingCallbacks();
    auto sourceWorkflow =
      portal::LibraryImportExportWorkflow{sourceFixture.runtime(), callbacks, ao::test::englishMessageCatalog()};
    auto targetWorkflow =
      portal::LibraryImportExportWorkflow{targetFixture.runtime(), callbacks, ao::test::englishMessageCatalog()};
    auto const target = sourceFixture.tempDir().path() / "roundtrip.yaml";

    copyMetadataFixtureToLibrary(sourceFixture);
    sourceWorkflow.scan();
    REQUIRE(pumpGtkEventsUntil(
      [&sourceFixture]
      { return hasNotification(sourceFixture, rt::NotificationSeverity::Info, "Library scan complete"); }));

    sourceWorkflow.exportTo(target, rt::ExportMode::Full);
    REQUIRE(pumpGtkEventsUntil(
      [&sourceFixture, &target]
      {
        return std::filesystem::exists(target) &&
               hasNotification(sourceFixture, rt::NotificationSeverity::Info, "Library exported successfully");
      }));

    copyMetadataFixtureToLibrary(targetFixture);

    targetWorkflow.importFrom(target);

    REQUIRE(pumpGtkEventsUntil(
      [&targetFixture]
      { return hasNotification(targetFixture, rt::NotificationSeverity::Info, "Library imported successfully"); }));

    CHECK(trackTitles(targetFixture) == std::vector<std::string>{"Test Title"});
    auto allTracks = ao::test::requireValue(targetFixture.runtime().sources().acquire(rt::kAllTracksListId));
    CHECK(allTracks->size() == 1);
  }

  TEST_CASE("LibraryImportExportWorkflow - restore waits for explicit preview confirmation",
            "[gtk][unit][workflow][import-confirmation]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto confirmation = std::function<void(bool)>{};
    auto optPreview = std::optional<rt::ImportReport>{};
    auto callbacks = portal::ImportExportCallbacks{
      .requestLibraryRestoreConfirmation =
        [&confirmation, &optPreview](rt::ImportReport const& report, std::function<void(bool)> completion)
      {
        optPreview = report;
        confirmation = std::move(completion);
      },
    };
    auto workflow =
      portal::LibraryImportExportWorkflow{fixture.runtime(), callbacks, ao::test::englishMessageCatalog()};
    auto const importPath = fixture.tempDir().path() / "restore.yaml";
    {
      auto yaml = std::ofstream{importPath};
      yaml << R"(version: 5
export_mode: full
library:
  resources: []
  tracks:
    - uri: restored.flac
      title: Restored
  lists: []
)";
    }

    workflow.importFrom(importPath);

    REQUIRE(pumpGtkEventsUntil([&confirmation] { return static_cast<bool>(confirmation); }));
    REQUIRE(optPreview);
    CHECK(optPreview->tracksCreated == 1);
    CHECK_FALSE(libraryHasTrackTitle(fixture, "Restored"));

    confirmation(false);
    drainGtkEvents();

    CHECK_FALSE(libraryHasTrackTitle(fixture, "Restored"));
  }

  TEST_CASE("LibraryImportExportWorkflow - confirmation becomes inert after workflow destruction",
            "[gtk][regression][workflow][concurrency]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto confirmation = std::function<void(bool)>{};
    auto callbacks = portal::ImportExportCallbacks{
      .requestLibraryRestoreConfirmation = [&confirmation](
                                             rt::ImportReport const&, std::function<void(bool)> completion)
      { confirmation = std::move(completion); },
    };
    auto const importPath = fixture.tempDir().path() / "restore.yaml";
    {
      auto yaml = std::ofstream{importPath};
      yaml << R"(version: 5
export_mode: full
library:
  resources: []
  tracks:
    - uri: restored.flac
      title: Restored
  lists: []
)";
    }

    {
      auto workflowPtr = std::make_unique<portal::LibraryImportExportWorkflow>(
        fixture.runtime(), callbacks, ao::test::englishMessageCatalog());
      workflowPtr->importFrom(importPath);
      REQUIRE(pumpGtkEventsUntil([&confirmation] { return static_cast<bool>(confirmation); }));
    }

    confirmation(true);
    drainGtkEvents();

    CHECK_FALSE(libraryHasTrackTitle(fixture, "Restored"));
    CHECK(fixture.runtime().notifications().feed().entries.empty());
  }

  TEST_CASE("LibraryImportExportWorkflow - destruction after commit suppresses frontend completion",
            "[gtk][regression][workflow][concurrency]")
  {
    auto tempDir = ao::test::TempDir{};
    auto executorPtr = std::make_unique<rt::test::ManualExecutor>();
    auto* const executor = executorPtr.get();
    auto runtimePtr = ao::test::requireValue(rt::AppRuntime::create(rt::AppRuntimeDependencies{
      .executorPtr = std::move(executorPtr),
      .musicRoot = tempDir.path() / "music",
      .databasePath = tempDir.path() / "db",
      .musicLibraryPinnedMapBytes = library::test::kTestMusicLibraryMapBytes,
      .workspaceConfigStorePtr = std::make_unique<rt::ConfigStore>(tempDir.path() / "config.yaml"),
    }));
    auto confirmation = std::function<void(bool)>{};
    auto callbacks = portal::ImportExportCallbacks{
      .requestLibraryRestoreConfirmation = [&confirmation](
                                             rt::ImportReport const&, std::function<void(bool)> completion)
      { confirmation = std::move(completion); },
    };
    auto const importPath = tempDir.path() / "restore.yaml";
    {
      auto yaml = std::ofstream{importPath};
      yaml << R"(version: 5
export_mode: full
library:
  resources: []
  tracks:
    - uri: restored.flac
      title: Restored
  lists: []
)";
    }

    auto workflowPtr =
      std::make_unique<portal::LibraryImportExportWorkflow>(*runtimePtr, callbacks, ao::test::englishMessageCatalog());
    workflowPtr->importFrom(importPath);

    while (!confirmation)
    {
      executor->checkQueued();
      REQUIRE(executor->runOne());
    }

    executor->runUntilIdle();
    bool publishedReset = false;
    auto changeSubscription = runtimePtr->library().changes().onChanged(
      [&publishedReset](rt::LibraryChangeSet const& changeSet) noexcept { publishedReset = changeSet.libraryReset; });
    confirmation(true);

    // Publication proves that the commit and runtime replica update completed.
    // Leave the later frontend hop pending so destruction can invalidate it.
    while (!publishedReset)
    {
      executor->checkQueued();
      REQUIRE(executor->runOne());
    }

    auto const trackIds = rt::test::runtimeTrackIds(*runtimePtr);
    REQUIRE(trackIds.size() == 1);
    auto snapshot = runtimePtr->library().snapshot();
    auto const optRestoredRow = snapshot.trackRow(trackIds.front());
    REQUIRE(optRestoredRow);
    CHECK(optRestoredRow->title == "Restored");

    workflowPtr.reset();
    executor->runUntilIdle();

    CHECK_FALSE(std::ranges::any_of(
      runtimePtr->notifications().feed().entries,
      [](auto const& entry) { return std::get<std::string>(entry.message) == "Library imported successfully"; }));
  }

  TEST_CASE("LibraryImportExportWorkflow - import reports read errors without mutation", "[gtk][unit][workflow][error]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto callbacks = confirmingCallbacks();
    auto workflow =
      portal::LibraryImportExportWorkflow{fixture.runtime(), callbacks, ao::test::englishMessageCatalog()};

    workflow.importFrom(fixture.tempDir().path() / "missing.yaml");

    REQUIRE(pumpGtkEventsUntil(
      [&fixture]
      {
        return hasNotificationContaining(fixture, rt::NotificationSeverity::Error, "Import failed: Failed to read");
      }));

    CHECK_FALSE(hasNotification(fixture, rt::NotificationSeverity::Error, "Import failed: Internal error"));
  }

  TEST_CASE("LibraryImportExportWorkflow - destruction cancels pending import without internal error",
            "[gtk][regression][workflow][concurrency]")
  {
    auto tempDir = ao::test::TempDir{};
    auto executorPtr = std::make_unique<rt::test::ManualExecutor>();
    auto* const executor = executorPtr.get();
    auto runtimePtr = ao::test::requireValue(rt::AppRuntime::create(rt::AppRuntimeDependencies{
      .executorPtr = std::move(executorPtr),
      .musicRoot = tempDir.path() / "music",
      .databasePath = tempDir.path() / "db",
      .musicLibraryPinnedMapBytes = library::test::kTestMusicLibraryMapBytes,
      .workspaceConfigStorePtr = std::make_unique<rt::ConfigStore>(tempDir.path() / "config.yaml"),
    }));

    auto callbacks = portal::ImportExportCallbacks{};
    auto const importPath = tempDir.path() / "missing-import.yaml";

    {
      auto workflowPtr = std::make_unique<portal::LibraryImportExportWorkflow>(
        *runtimePtr, callbacks, ao::test::englishMessageCatalog());
      workflowPtr->importFrom(importPath);

      executor->checkQueued();
      REQUIRE(executor->runOne());
      executor->checkQueued(std::chrono::seconds{2});

      workflowPtr.reset();
    }

    executor->runUntilIdle();

    CHECK(runtimePtr->notifications().feed().entries.empty());
  }
} // namespace ao::gtk::test
