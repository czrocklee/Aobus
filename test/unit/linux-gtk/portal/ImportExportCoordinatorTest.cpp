// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "portal/ImportExportCoordinator.h"

#include "app/ThemeCoordinator.h"
#include "portal/ImportExportCallbacks.h"
#include "test/unit/linux-gtk/GtkTestSupport.h"
#include <ao/async/Executor.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ConfigStore.h>
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
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
#include <utility>

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

    class QueuedExecutor final : public async::IExecutor
    {
    public:
      bool isCurrent() const noexcept override { return true; }

      void dispatch(std::move_only_function<void()> task) override
      {
        auto const lock = std::scoped_lock{_mutex};
        _tasks.push_back(std::move(task));
      }

      void defer(std::move_only_function<void()> task) override { dispatch(std::move(task)); }

      bool runOne()
      {
        auto task = std::move_only_function<void()>{};

        {
          auto const lock = std::scoped_lock{_mutex};

          if (_tasks.empty())
          {
            return false;
          }

          task = std::move(_tasks.front());
          _tasks.pop_front();
        }

        task();
        return true;
      }

      bool waitUntilQueued(std::chrono::milliseconds timeout = std::chrono::milliseconds{500}) const
      {
        auto const deadline = std::chrono::steady_clock::now() + timeout;

        while (std::chrono::steady_clock::now() < deadline)
        {
          {
            auto const lock = std::scoped_lock{_mutex};

            if (!_tasks.empty())
            {
              return true;
            }
          }

          std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }

        auto const lock = std::scoped_lock{_mutex};
        return !_tasks.empty();
      }

    private:
      mutable std::mutex _mutex;
      std::deque<std::move_only_function<void()>> _tasks;
    };
  } // namespace

  TEST_CASE("ImportExportCoordinator - openMusicLibrary routes to the callback", "[gtk][portal][import-export]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};

    auto parent = Gtk::Window{};
    auto theme = ThemeCoordinator{};

    auto receivedPath = std::filesystem::path{};
    bool receivedScanAfterOpen = false;
    std::int32_t mutationCallbackCount = 0;
    auto callbacks = ImportExportCallbacks{
      .onOpenNewLibrary =
        [&receivedPath, &receivedScanAfterOpen](std::filesystem::path const& path, bool const scanAfterOpen)
      {
        receivedPath = path;
        receivedScanAfterOpen = scanAfterOpen;
      },
      .onLibraryDataMutated = [&mutationCallbackCount] { ++mutationCallbackCount; },
      .onTitleChanged = {},
    };

    auto coordinator = ImportExportCoordinator{parent, fixture.runtime(), callbacks, theme};

    SECTION("openMusicLibrary invokes the onOpenNewLibrary callback")
    {
      auto const target = std::filesystem::path{fixture.tempDir().path() / "new_library"};
      coordinator.openMusicLibrary(target);
      CHECK(receivedPath == target);
      CHECK_FALSE(receivedScanAfterOpen);
    }

    SECTION("openMusicLibrary forwards the initial scan request")
    {
      auto const target = std::filesystem::path{fixture.tempDir().path() / "new_library"};
      coordinator.openMusicLibrary(target, true);
      CHECK(receivedPath == target);
      CHECK(receivedScanAfterOpen);
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

    SECTION("scanLibrary completes after scanning unchanged files")
    {
      auto const sourceFile = audio::test::requireAudioFixture("basic_metadata.flac");
      std::filesystem::copy_file(sourceFile, fixture.runtime().musicRoot() / "song.flac");

      auto optCompletedCount = std::optional<std::size_t>{};
      bool progressFired = false;
      auto completedSub = fixture.runtime().library().changes().onLibraryTaskCompleted(
        [&optCompletedCount](std::size_t count) { optCompletedCount = count; });
      auto progressSub = fixture.runtime().library().changes().onLibraryTaskProgress([&progressFired](auto const&)
                                                                                     { progressFired = true; });

      coordinator.scanLibrary();
      REQUIRE(drainGtkEventsUntil(
        [&fixture, &mutationCallbackCount, &optCompletedCount]
        {
          return optCompletedCount.has_value() && mutationCallbackCount == 1 &&
                 hasNotification(fixture, rt::NotificationSeverity::Info, "Library scan complete");
        }));
      CHECK(optCompletedCount == 1U);
      CHECK(mutationCallbackCount == 1);

      optCompletedCount.reset();
      progressFired = false;
      fixture.runtime().notifications().dismissAll();

      coordinator.scanLibrary();

      REQUIRE(drainGtkEventsUntil(
        [&fixture, &optCompletedCount, &progressFired]
        {
          return progressFired && optCompletedCount.has_value() &&
                 hasNotification(fixture, rt::NotificationSeverity::Info, "Library is up to date");
        }));

      CHECK(optCompletedCount == 0U);
      CHECK(mutationCallbackCount == 1);
    }

    SECTION("scanLibrary reports an error-only plan instead of up to date")
    {
      auto const restrictedDir = fixture.runtime().musicRoot() / "restricted_dir";
      std::filesystem::create_directories(restrictedDir);
      std::filesystem::permissions(restrictedDir, std::filesystem::perms::none);

      if (::geteuid() == 0)
      {
        SKIP("permissions test is meaningless when running as root");
      }

      coordinator.scanLibrary();

      REQUIRE(drainGtkEventsUntil(
        [&fixture] { return hasNotification(fixture, rt::NotificationSeverity::Error, "Scan failed"); }));

      std::filesystem::permissions(restrictedDir, std::filesystem::perms::owner_all);

      CHECK_FALSE(hasNotification(fixture, rt::NotificationSeverity::Info, "Library is up to date"));
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
        {
          return hasNotificationContaining(fixture, rt::NotificationSeverity::Error, "Import failed: Failed to read");
        }));

      CHECK(mutationCallbackCount == 0);
      CHECK_FALSE(hasNotification(fixture, rt::NotificationSeverity::Error, "Import failed: Internal error"));
    }
  }

  TEST_CASE("ImportExportCoordinator - lifetime cancellation does not report an internal error",
            "[gtk][portal][import-export]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto tempDir = ao::test::TempDir{};
    auto executorPtr = std::make_unique<QueuedExecutor>();
    auto* const executor = executorPtr.get();
    auto runtime = rt::AppRuntime{rt::AppRuntimeDependencies{
      .executorPtr = std::move(executorPtr),
      .musicRoot = tempDir.path() / "music",
      .databasePath = tempDir.path() / "db",
      .workspaceConfigStorePtr = std::make_unique<rt::ConfigStore>(tempDir.path() / "config.yaml"),
    }};

    auto parent = Gtk::Window{};
    auto theme = ThemeCoordinator{};
    auto callbacks = ImportExportCallbacks{};

    auto const importPath = tempDir.path() / "missing-import.yaml";

    {
      auto coordinatorPtr = std::make_unique<ImportExportCoordinator>(parent, runtime, callbacks, theme);
      coordinatorPtr->importLibraryFrom(importPath);

      REQUIRE(executor->waitUntilQueued());
      REQUIRE(executor->runOne());
      REQUIRE(executor->waitUntilQueued(std::chrono::seconds{2}));

      coordinatorPtr.reset();
    }

    while (executor->runOne())
    {
    }

    CHECK(runtime.notifications().feed().entries.empty());
  }
} // namespace ao::gtk::test
