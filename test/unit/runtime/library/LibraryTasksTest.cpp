// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "test/unit/RuntimeTestUtils.h"
#include <ao/Error.h>
#include <ao/async/Runtime.h>
#include <ao/async/Task.h>
#include <ao/library/LibraryScanner.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryTasks.h>
#include <ao/rt/library/LibraryYamlExporter.h>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::rt::test
{
  TEST_CASE("LibraryTasks - importLibraryAsync returns failure for invalid path", "[runtime][unit][library][task]")
  {
    auto testLib = TestMusicLibrary{};
    auto executor = MockExecutor{};
    auto runtime = async::Runtime{executor};
    auto changes = LibraryChanges{};
    auto service = LibraryTasks{runtime, testLib.library(), changes};

    auto future = runtime.spawn(service.importLibraryAsync("/nonexistent_path_123.yaml"));
    auto const result = future.get();

    REQUIRE_FALSE(result);
    CHECK(result.error().code == Error::Code::IoError);
    CHECK(result.error().message.find("Failed to read") != std::string::npos);
    CHECK(std::string_view{result.error().location.file_name()}.find("LibraryYamlImporter.cpp") !=
          std::string_view::npos);
  }

  TEST_CASE("LibraryTasks - exportLibraryAsync returns failure for invalid path", "[runtime][unit][library][task]")
  {
    auto testLib = TestMusicLibrary{};
    auto executor = MockExecutor{};
    auto runtime = async::Runtime{executor};
    auto changes = LibraryChanges{};
    auto service = LibraryTasks{runtime, testLib.library(), changes};

    auto future = runtime.spawn(service.exportLibraryAsync("/root/nonexistent_path_123.yaml", ExportMode::Full));
    auto const result = future.get();

    REQUIRE_FALSE(result);
    CHECK(result.error().code == Error::Code::IoError);
  }

  TEST_CASE("LibraryTasks - buildScanPlanAsync succeeds", "[runtime][unit][library][task][scan]")
  {
    auto testLib = TestMusicLibrary{};
    auto executor = MockExecutor{};
    auto runtime = async::Runtime{executor};
    auto changes = LibraryChanges{};
    auto service = LibraryTasks{runtime, testLib.library(), changes};

    auto future = runtime.spawn(service.buildScanPlanAsync());
    auto const result = future.get();

    REQUIRE(result);
  }

  TEST_CASE("LibraryTasks - applyScanPlanAsync succeeds with empty plan", "[runtime][unit][library][task][scan]")
  {
    auto testLib = TestMusicLibrary{};
    auto executor = MockExecutor{};
    auto runtime = async::Runtime{executor};
    auto changes = LibraryChanges{};
    auto service = LibraryTasks{runtime, testLib.library(), changes};

    auto plan = library::ScanPlan{};
    auto future = runtime.spawn(service.applyScanPlanAsync(std::move(plan)));
    auto const result = future.get();

    REQUIRE(result);
    CHECK(result->processedIds.empty());
    CHECK(result->failureCount == 0);
  }

  TEST_CASE("LibraryTasks - applyScanPlanAsync reports progress while applying plan",
            "[runtime][unit][library][task][scan]")
  {
    auto testLib = TestMusicLibrary{};
    auto executor = MockExecutor{};
    auto runtime = async::Runtime{executor};
    auto changes = LibraryChanges{};
    auto service = LibraryTasks{runtime, testLib.library(), changes};

    auto progressEvents = std::vector<LibraryChanges::LibraryTaskProgressUpdated>{};
    auto sub = changes.onLibraryTaskProgress([&](auto const& ev) { progressEvents.push_back(ev); });

    auto wrapperTask = [](LibraryTasks* s) -> async::Task<Result<library::ScanApplyResult>>
    {
      auto plan = library::ScanPlan{};
      auto firstItem = library::ScanItem{};
      firstItem.uri = "file:///fake/first.flac";
      firstItem.fullPath = "/fake/first.flac";
      firstItem.classification = library::ScanClassification::New;
      plan.items.push_back(firstItem);

      auto secondItem = library::ScanItem{};
      secondItem.uri = "file:///fake/second.flac";
      secondItem.fullPath = "/fake/second.flac";
      secondItem.classification = library::ScanClassification::New;
      plan.items.push_back(secondItem);
      co_return co_await s->applyScanPlanAsync(std::move(plan));
    };

    auto future = runtime.spawn(wrapperTask(&service));
    auto const result = future.get();

    REQUIRE(result);
    CHECK(result->processedIds.empty());
    CHECK(result->failureCount == 2);

    REQUIRE(progressEvents.size() == 2);
    CHECK(progressEvents[0].message == "Updating: first.flac");
    CHECK(progressEvents[0].fraction == 0.0);
    CHECK(progressEvents[1].message == "Updating: second.flac");
    CHECK(progressEvents[1].fraction == 0.5);

    for (auto const& event : progressEvents)
    {
      CHECK(event.fraction >= 0.0);
      CHECK(event.fraction <= 1.0);
      CHECK_FALSE(event.message.empty());
    }
  }
} // namespace ao::rt::test
