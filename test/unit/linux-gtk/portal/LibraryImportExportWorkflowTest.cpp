// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "portal/LibraryImportExportWorkflow.h"

#include "portal/ImportExportCallbacks.h"
#include "test/unit/RuntimeTestUtils.h"
#include "test/unit/audio/AudioFixtureUtils.h"
#include "test/unit/linux-gtk/GtkTestSupport.h"
#include <ao/library/TrackStore.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ConfigStore.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/StateTypes.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryYamlExporter.h>

#include <catch2/catch_test_macros.hpp>
#include <unistd.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
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
        feed.entries, [&](auto const& entry) { return entry.severity == severity && entry.message == message; });
    }

    bool hasNotificationContaining(GtkRuntimeFixture& fixture,
                                   rt::NotificationSeverity severity,
                                   std::string_view messageFragment)
    {
      auto const feed = fixture.runtime().notifications().feed();

      return std::ranges::any_of(feed.entries,
                                 [&](auto const& entry)
                                 {
                                   return entry.severity == severity && std::string_view{entry.message}.find(
                                                                          messageFragment) != std::string_view::npos;
                                 });
    }

    portal::ImportExportCallbacks callbacksWithMutationCounter(std::int32_t& mutationCallbackCount)
    {
      return portal::ImportExportCallbacks{
        .onOpenNewLibrary = {},
        .onLibraryDataMutated = [&mutationCallbackCount] { ++mutationCallbackCount; },
        .onTitleChanged = {},
      };
    }

    std::vector<std::string> trackTitles(GtkRuntimeFixture& fixture)
    {
      auto titles = std::vector<std::string>{};
      auto txn = fixture.runtime().musicLibrary().readTransaction();
      auto reader = fixture.runtime().musicLibrary().tracks().reader(txn);

      for (auto const& [id, view] : reader)
      {
        std::ignore = id;
        titles.emplace_back(view.metadata().title());
      }

      return titles;
    }

    bool libraryHasTrackTitle(GtkRuntimeFixture& fixture, std::string_view expectedTitle)
    {
      auto const titles = trackTitles(fixture);
      return std::ranges::any_of(titles, [&](std::string const& title) { return title == expectedTitle; });
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

  TEST_CASE("LibraryImportExportWorkflow - scan reports up-to-date empty library", "[gtk][unit][workflow]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    std::int32_t mutationCallbackCount = 0;
    auto callbacks = callbacksWithMutationCounter(mutationCallbackCount);
    auto workflow = portal::LibraryImportExportWorkflow{fixture.runtime(), callbacks};

    auto optCompletedCount = std::optional<std::size_t>{};
    auto completedSub = fixture.runtime().library().changes().onLibraryTaskCompleted(
      [&optCompletedCount](std::size_t count) { optCompletedCount = count; });

    workflow.scan();

    REQUIRE(pumpGtkEventsUntil(
      [&fixture, &optCompletedCount]
      { return optCompletedCount.has_value() && !fixture.runtime().notifications().feed().entries.empty(); }));

    CHECK(optCompletedCount == 0U);
    CHECK(mutationCallbackCount == 0);
    auto const feed = fixture.runtime().notifications().feed();
    REQUIRE(feed.entries.size() == 1);
    CHECK(feed.entries.back().severity == rt::NotificationSeverity::Info);
    CHECK(feed.entries.back().message == "Library is up to date");
  }

  TEST_CASE("LibraryImportExportWorkflow - scan mutates only when files change", "[gtk][unit][workflow]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    std::int32_t mutationCallbackCount = 0;
    auto callbacks = callbacksWithMutationCounter(mutationCallbackCount);
    auto workflow = portal::LibraryImportExportWorkflow{fixture.runtime(), callbacks};

    copyMetadataFixtureToLibrary(fixture);

    auto optCompletedCount = std::optional<std::size_t>{};
    bool progressFired = false;
    auto completedSub = fixture.runtime().library().changes().onLibraryTaskCompleted(
      [&optCompletedCount](std::size_t count) { optCompletedCount = count; });
    auto progressSub = fixture.runtime().library().changes().onLibraryTaskProgress([&progressFired](auto const&)
                                                                                   { progressFired = true; });

    workflow.scan();
    REQUIRE(pumpGtkEventsUntil(
      [&fixture, &mutationCallbackCount, &optCompletedCount]
      {
        return optCompletedCount.has_value() && mutationCallbackCount == 1 &&
               hasNotification(fixture, rt::NotificationSeverity::Info, "Library scan complete");
      }));
    CHECK(optCompletedCount == 1U);
    CHECK(mutationCallbackCount == 1);
    CHECK(trackTitles(fixture) == std::vector<std::string>{"Test Title"});

    optCompletedCount.reset();
    progressFired = false;
    fixture.runtime().notifications().dismissAll();

    workflow.scan();

    REQUIRE(pumpGtkEventsUntil(
      [&fixture, &optCompletedCount, &progressFired]
      {
        return progressFired && optCompletedCount.has_value() &&
               hasNotification(fixture, rt::NotificationSeverity::Info, "Library is up to date");
      }));

    CHECK(optCompletedCount == 0U);
    CHECK(mutationCallbackCount == 1);
  }

  TEST_CASE("LibraryImportExportWorkflow - scan reports error-only plans without up-to-date success",
            "[gtk][unit][workflow][error]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    std::int32_t mutationCallbackCount = 0;
    auto callbacks = callbacksWithMutationCounter(mutationCallbackCount);
    auto workflow = portal::LibraryImportExportWorkflow{fixture.runtime(), callbacks};

    auto const restrictedDir = fixture.runtime().musicRoot() / "restricted_dir";
    std::filesystem::create_directories(restrictedDir);
    std::filesystem::permissions(restrictedDir, std::filesystem::perms::none);

    if (::geteuid() == 0)
    {
      SKIP("permissions test is meaningless when running as root");
    }

    workflow.scan();

    REQUIRE(pumpGtkEventsUntil([&fixture]
                               { return hasNotification(fixture, rt::NotificationSeverity::Error, "Scan failed"); }));

    std::filesystem::permissions(restrictedDir, std::filesystem::perms::owner_all);

    CHECK(mutationCallbackCount == 0);
    CHECK_FALSE(hasNotification(fixture, rt::NotificationSeverity::Info, "Library is up to date"));
  }

  TEST_CASE("LibraryImportExportWorkflow - export writes scanned track metadata to YAML", "[gtk][unit][workflow][yaml]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    std::int32_t mutationCallbackCount = 0;
    auto callbacks = callbacksWithMutationCounter(mutationCallbackCount);
    auto workflow = portal::LibraryImportExportWorkflow{fixture.runtime(), callbacks};
    auto const target = fixture.tempDir().path() / "library_backup.yaml";

    copyMetadataFixtureToLibrary(fixture);
    workflow.scan();
    REQUIRE(pumpGtkEventsUntil(
      [&fixture] { return hasNotification(fixture, rt::NotificationSeverity::Info, "Library scan complete"); }));
    REQUIRE(libraryHasTrackTitle(fixture, "Test Title"));
    fixture.runtime().notifications().dismissAll();
    mutationCallbackCount = 0;

    workflow.exportTo(target, rt::ExportMode::Full);

    REQUIRE(pumpGtkEventsUntil(
      [&fixture, &target]
      {
        return std::filesystem::exists(target) &&
               hasNotification(fixture, rt::NotificationSeverity::Info, "Library exported successfully");
      }));

    auto const exportedYaml = readTextFile(target);
    CHECK(std::string_view{exportedYaml}.find("Test Title") != std::string_view::npos);
    CHECK(std::string_view{exportedYaml}.find("Test Artist") != std::string_view::npos);
    CHECK(mutationCallbackCount == 0);
  }

  TEST_CASE("LibraryImportExportWorkflow - import restores track metadata and posts mutation callback",
            "[gtk][unit][workflow][yaml]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto sourceFixture = GtkRuntimeFixture{};
    auto targetFixture = GtkRuntimeFixture{};
    std::int32_t mutationCallbackCount = 0;
    auto callbacks = callbacksWithMutationCounter(mutationCallbackCount);
    auto sourceWorkflow = portal::LibraryImportExportWorkflow{sourceFixture.runtime(), callbacks};
    auto targetWorkflow = portal::LibraryImportExportWorkflow{targetFixture.runtime(), callbacks};
    auto const target = sourceFixture.tempDir().path() / "roundtrip.yaml";

    copyMetadataFixtureToLibrary(sourceFixture);
    sourceWorkflow.scan();
    REQUIRE(pumpGtkEventsUntil(
      [&sourceFixture]
      { return hasNotification(sourceFixture, rt::NotificationSeverity::Info, "Library scan complete"); }));
    sourceFixture.runtime().notifications().dismissAll();

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
      [&targetFixture, &mutationCallbackCount]
      {
        return mutationCallbackCount == 2 &&
               hasNotification(targetFixture, rt::NotificationSeverity::Info, "Library imported successfully");
      }));

    CHECK(mutationCallbackCount == 2);
    CHECK(trackTitles(targetFixture) == std::vector<std::string>{"Test Title"});
  }

  TEST_CASE("LibraryImportExportWorkflow - import reports read errors without mutation", "[gtk][unit][workflow][error]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    std::int32_t mutationCallbackCount = 0;
    auto callbacks = callbacksWithMutationCounter(mutationCallbackCount);
    auto workflow = portal::LibraryImportExportWorkflow{fixture.runtime(), callbacks};

    workflow.importFrom(fixture.tempDir().path() / "missing.yaml");

    REQUIRE(pumpGtkEventsUntil(
      [&fixture]
      {
        return hasNotificationContaining(fixture, rt::NotificationSeverity::Error, "Import failed: Failed to read");
      }));

    CHECK(mutationCallbackCount == 0);
    CHECK_FALSE(hasNotification(fixture, rt::NotificationSeverity::Error, "Import failed: Internal error"));
  }

  TEST_CASE("LibraryImportExportWorkflow - destruction cancels pending import without internal error",
            "[gtk][unit][workflow]")
  {
    auto tempDir = ao::test::TempDir{};
    auto executorPtr = std::make_unique<rt::test::ManualExecutor>();
    auto* const executor = executorPtr.get();
    auto runtime = rt::AppRuntime{rt::AppRuntimeDependencies{
      .executorPtr = std::move(executorPtr),
      .musicRoot = tempDir.path() / "music",
      .databasePath = tempDir.path() / "db",
      .workspaceConfigStorePtr = std::make_unique<rt::ConfigStore>(tempDir.path() / "config.yaml"),
    }};

    auto callbacks = portal::ImportExportCallbacks{};
    auto const importPath = tempDir.path() / "missing-import.yaml";

    {
      auto workflowPtr = std::make_unique<portal::LibraryImportExportWorkflow>(runtime, callbacks);
      workflowPtr->importFrom(importPath);

      executor->expectQueued();
      REQUIRE(executor->runOne());
      executor->expectQueued(std::chrono::seconds{2});

      workflowPtr.reset();
    }

    executor->runUntilIdle();

    CHECK(runtime.notifications().feed().entries.empty());
  }
} // namespace ao::gtk::test
