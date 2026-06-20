// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "portal/ImportExportCoordinator.h"

#include "app/ThemeCoordinator.h"
#include "test/unit/linux-gtk/GtkTestSupport.h"
#include <ao/rt/AppRuntime.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/StateTypes.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryYamlExporter.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/window.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>
#include <thread>

namespace ao::gtk::test
{
  using ao::gtk::portal::ImportExportCallbacks;
  using ao::gtk::portal::ImportExportCoordinator;

  namespace
  {
    template<typename Predicate>
    bool drainGtkEventsUntil(Predicate const& predicate,
                             std::chrono::milliseconds timeout = std::chrono::milliseconds{500})
    {
      auto const deadline = std::chrono::steady_clock::now() + timeout;

      while (std::chrono::steady_clock::now() < deadline)
      {
        drainGtkEvents();

        if (predicate())
        {
          return true;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds{1});
      }

      drainGtkEvents();
      return predicate();
    }

    bool hasNotification(GtkRuntimeFixture& fixture, rt::NotificationSeverity severity, std::string_view message)
    {
      auto const feed = fixture.runtime().notifications().feed();

      return std::ranges::any_of(
        feed.entries, [&](auto const& entry) { return entry.severity == severity && entry.message == message; });
    }
  } // namespace

  TEST_CASE("ImportExportCoordinator - openMusicLibrary routes to the callback", "[gtk][portal][import-export]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};

    auto parent = Gtk::Window{};
    auto theme = ThemeCoordinator{};

    auto receivedPath = std::filesystem::path{};
    std::int32_t mutationCallbackCount = 0;
    auto callbacks = ImportExportCallbacks{
      .onOpenNewLibrary = [&receivedPath](std::filesystem::path const& path) { receivedPath = path; },
      .onLibraryDataMutated = [&mutationCallbackCount] { ++mutationCallbackCount; },
      .onTitleChanged = {},
    };

    auto coordinator = ImportExportCoordinator{parent, fixture.runtime(), callbacks, theme};

    SECTION("openMusicLibrary invokes the onOpenNewLibrary callback")
    {
      auto const target = std::filesystem::path{fixture.tempDir().path() / "new_library"};
      coordinator.openMusicLibrary(target);
      CHECK(receivedPath == target);
    }

    SECTION("scanLibrary reports an up-to-date empty library")
    {
      auto optCompletedCount = std::optional<std::size_t>{};
      auto completedSub = fixture.runtime().library().changes().onLibraryTaskCompleted(
        [&optCompletedCount](std::size_t count) { optCompletedCount = count; });

      coordinator.scanLibrary();

      REQUIRE(drainGtkEventsUntil(
        [&fixture, &optCompletedCount]
        { return optCompletedCount.has_value() && !fixture.runtime().notifications().feed().entries.empty(); }));

      CHECK(optCompletedCount == 0U);
      auto const feed = fixture.runtime().notifications().feed();
      REQUIRE(feed.entries.size() == 1);
      CHECK(feed.entries.back().severity == rt::NotificationSeverity::Info);
      CHECK(feed.entries.back().message == "Library is up to date");
    }

    SECTION("exportLibraryTo writes the YAML backup and reports success")
    {
      auto const target = fixture.tempDir().path() / "library_backup.yaml";

      coordinator.exportLibraryTo(target, rt::ExportMode::Full);

      REQUIRE(drainGtkEventsUntil(
        [&fixture, &target]
        {
          return std::filesystem::exists(target) &&
                 hasNotification(fixture, rt::NotificationSeverity::Info, "Library exported successfully");
        }));

      CHECK(std::filesystem::file_size(target) > 0U);
    }

    SECTION("importLibraryFrom imports a selected YAML file and reports mutation")
    {
      auto const target = fixture.tempDir().path() / "roundtrip.yaml";

      coordinator.exportLibraryTo(target, rt::ExportMode::Full);
      REQUIRE(drainGtkEventsUntil(
        [&fixture, &target]
        {
          return std::filesystem::exists(target) &&
                 hasNotification(fixture, rt::NotificationSeverity::Info, "Library exported successfully");
        }));

      fixture.runtime().notifications().dismissAll();

      coordinator.importLibraryFrom(target);

      REQUIRE(drainGtkEventsUntil(
        [&fixture, &mutationCallbackCount]
        {
          return mutationCallbackCount == 1 &&
                 hasNotification(fixture, rt::NotificationSeverity::Info, "Library imported successfully");
        }));

      CHECK(mutationCallbackCount == 1);
    }

    SECTION("importLibraryFrom reports errors without mutating the library")
    {
      coordinator.importLibraryFrom(fixture.tempDir().path() / "missing.yaml");

      REQUIRE(drainGtkEventsUntil(
        [&fixture]
        { return hasNotification(fixture, rt::NotificationSeverity::Error, "Import failed: Internal error"); }));

      CHECK(mutationCallbackCount == 0);
    }
  }
} // namespace ao::gtk::test
