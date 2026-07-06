// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "test/unit/RuntimeTestUtils.h"
#include "test/unit/audio/AudioFixtureUtils.h"
#include <ao/Error.h>
#include <ao/async/OperationCancelled.h>
#include <ao/async/Runtime.h>
#include <ao/async/Task.h>
#include <ao/library/AudioIdentity.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/TrackStore.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryScan.h>
#include <ao/rt/library/LibraryTasks.h>
#include <ao/rt/library/LibraryYamlExporter.h>
#include <ao/rt/library/ScanPlan.h>

#include <catch2/catch_test_macros.hpp>

#include <exception>
#include <filesystem>
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

    auto plan = ScanPlan{};
    auto future = runtime.spawn(service.applyScanPlanAsync(std::move(plan)));
    auto const result = future.get();

    REQUIRE(result);
    CHECK(result->processedIds.empty());
    CHECK(result->failureCount == 0);
  }

  TEST_CASE("LibraryTasks - applyScanPlanAsync can defer new audio identity", "[runtime][unit][library][task]")
  {
    auto testLib = TestMusicLibrary{};
    auto const sourceFile = audio::test::requireAudioFixture("basic_metadata.flac");
    std::filesystem::copy_file(sourceFile, testLib.root() / "song.flac");

    auto executor = MockExecutor{};
    auto runtime = async::Runtime{executor};
    auto changes = LibraryChanges{};
    auto service = LibraryTasks{runtime, testLib.library(), changes};

    auto scanService = LibraryScan{testLib.library()};
    auto plan = scanService.buildPlan().value();
    REQUIRE(plan.count(ScanClassification::New) == 1);

    auto future = runtime.spawn(service.applyScanPlanAsync(
      std::move(plan), ScanApplyOptions{.audioIdentityPolicy = AudioIdentityPolicy::DeferNew}));
    auto const result = future.get();

    REQUIRE(result);
    REQUIRE(result->processedIds.size() == 1);
    auto txn = testLib.library().readTransaction();
    auto manifestResult = testLib.library().manifest().reader(txn).get("song.flac");
    REQUIRE(manifestResult);
    CHECK_FALSE(library::hasAudioIdentity(manifestResult->audioPayloadLength(), manifestResult->audioSignature()));
  }

  TEST_CASE("LibraryTasks - backfillAudioIdentityAsync fills pending rows", "[runtime][unit][library][task]")
  {
    auto testLib = TestMusicLibrary{};
    auto const sourceFile = audio::test::requireAudioFixture("basic_metadata.flac");
    std::filesystem::copy_file(sourceFile, testLib.root() / "song.flac");

    auto executor = MockExecutor{};
    auto runtime = async::Runtime{executor};
    auto changes = LibraryChanges{};
    auto service = LibraryTasks{runtime, testLib.library(), changes};

    auto scanService = LibraryScan{testLib.library()};
    auto plan = scanService.buildPlan().value();
    auto applyFuture = runtime.spawn(service.applyScanPlanAsync(
      std::move(plan), ScanApplyOptions{.audioIdentityPolicy = AudioIdentityPolicy::DeferNew}));
    REQUIRE(applyFuture.get());

    auto backfillFuture = runtime.spawn(service.backfillAudioIdentityAsync());
    auto const backfillResult = backfillFuture.get();

    REQUIRE(backfillResult);
    CHECK(backfillResult->completedCount == 1);
    CHECK(backfillResult->skippedCount == 0);
    CHECK(backfillResult->failureCount == 0);
    CHECK_FALSE(backfillResult->cancelled);

    auto txn = testLib.library().readTransaction();
    auto manifestResult = testLib.library().manifest().reader(txn).get("song.flac");
    REQUIRE(manifestResult);
    CHECK(library::hasAudioIdentity(manifestResult->audioPayloadLength(), manifestResult->audioSignature()));
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

    auto wrapperTask = [](LibraryTasks* s) -> async::Task<Result<ScanApplyResult>>
    {
      auto plan = ScanPlan{};
      auto firstItem = ScanItem{};
      firstItem.uri = "file:///fake/first.flac";
      firstItem.fullPath = "/fake/first.flac";
      firstItem.classification = ScanClassification::New;
      plan.items.push_back(firstItem);

      auto secondItem = ScanItem{};
      secondItem.uri = "file:///fake/second.flac";
      secondItem.fullPath = "/fake/second.flac";
      secondItem.classification = ScanClassification::New;
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

  TEST_CASE("LibraryTasks - applyScanPlanAsync forwards cancellation to scan executor",
            "[runtime][unit][library][task][scan][cancellation]")
  {
    auto testLib = TestMusicLibrary{};
    auto const sourceFile = audio::test::requireAudioFixture("basic_metadata.flac");
    std::filesystem::copy_file(sourceFile, testLib.root() / "song.flac");

    auto executor = MockExecutor{};
    auto runtime = async::Runtime{executor};
    auto changes = LibraryChanges{};
    auto service = LibraryTasks{runtime, testLib.library(), changes};

    auto scanService = LibraryScan{testLib.library()};
    auto plan = scanService.buildPlan().value();
    REQUIRE(plan.count(ScanClassification::New) == 1);

    auto signal = async::CancellationSignal{};
    auto sawFingerprinting = AsyncTestState<bool>::create(false);
    auto sawCancellation = AsyncTestState<bool>::create(false);
    auto sub = changes.onLibraryTaskProgress(
      [&](LibraryChanges::LibraryTaskProgressUpdated const& event)
      {
        if (event.message == "Fingerprinting: song.flac" && !sawFingerprinting.get())
        {
          sawFingerprinting.set(true);
          signal.emit(async::CancellationType::all);
        }
      });

    auto task = [](LibraryTasks* tasks, ScanPlan scanPlan) -> async::Task<void>
    { [[maybe_unused]] auto result = co_await tasks->applyScanPlanAsync(std::move(scanPlan)); };

    runtime.spawn(task(&service, std::move(plan)),
                  signal.slot(),
                  [sawCancellation](std::exception_ptr exPtr) mutable
                  {
                    try
                    {
                      if (exPtr)
                      {
                        std::rethrow_exception(exPtr);
                      }
                    }
                    catch (async::OperationCancelled const&)
                    {
                      sawCancellation.set(true);
                    }
                    catch (std::exception const& e)
                    {
                      if (async::isOperationCancelled(e))
                      {
                        sawCancellation.set(true);
                      }
                    }
                  });

    REQUIRE(sawCancellation.waitUntil(true));
    CHECK(sawFingerprinting.get());

    auto txn = testLib.library().readTransaction();
    auto trackReader = testLib.library().tracks().reader(txn);
    auto manifestReader = testLib.library().manifest().reader(txn);
    CHECK(trackReader.begin() == trackReader.end());
    CHECK(manifestReader.begin() == manifestReader.end());

    runtime.requestStop();
    runtime.join();
  }
} // namespace ao::rt::test
