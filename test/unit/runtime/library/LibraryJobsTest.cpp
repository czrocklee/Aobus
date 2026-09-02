// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/library/LibraryJobs.h>

#include "runtime/library/LibraryYamlImporter.h"
#include "runtime/library/ScanApplyOperation.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/library/WritableLibraryTestSupport.h"
#include "test/unit/runtime/AsyncTestSupport.h"
#include "test/unit/runtime/ExecutorTestSupport.h"
#include "test/unit/runtime/RuntimeLibraryTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/OperationCancelled.h>
#include <ao/async/Runtime.h>
#include <ao/async/Task.h>
#include <ao/async/TaskFuture.h>
#include <ao/library/AudioIdentity.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/TrackStore.h>
#include <ao/rt/ListMutation.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/library/AudioIdentityIndex.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryCommands.h>
#include <ao/rt/library/LibraryScan.h>
#include <ao/rt/library/LibraryTaskEvents.h>
#include <ao/rt/library/LibraryTransfer.h>
#include <ao/rt/library/ScanPlan.h>
#include <ao/utility/Path.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    template<typename Future>
    void requireCancellation(Future& future, async::Runtime& runtime)
    {
      auto const exceptionPtr = captureTaskFutureException(future);
      REQUIRE(exceptionPtr);

      runtime.requestStop();
      runtime.join();

      bool sawCancellation = false;

      try
      {
        std::rethrow_exception(exceptionPtr);
      }
      catch (async::OperationCancelled const&)
      {
        sawCancellation = true;
      }

      REQUIRE(sawCancellation);
    }

    async::Task<void> applyScanPlanAndRecordCancellation(LibraryJobs* jobs,
                                                         ScanPlan plan,
                                                         AsyncTestState<bool> fingerprintingEntered,
                                                         AsyncBarrier* fingerprintingRelease,
                                                         AsyncTestState<bool> sawCancellation,
                                                         std::stop_token const stopToken)
    {
      try
      {
        [[maybe_unused]] auto result = co_await jobs->applyScanPlanAsync(
          std::move(plan),
          {},
          stopToken,
          [fingerprintingEntered, fingerprintingRelease](ScanApplyProgress const& progress)
          {
            if (progress.stage == ScanApplyProgressStage::Fingerprinting && !fingerprintingEntered.load())
            {
              fingerprintingEntered.set(true);
              fingerprintingRelease->wait();
            }
          });
      }
      catch (async::OperationCancelled const&)
      {
        sawCancellation.set(true);
        throw;
      }
    }

    void writeImportPayload(std::filesystem::path const& path, std::string_view title)
    {
      auto yaml = std::ofstream{path};
      yaml << "version: 5\n"
              "export_mode: full\n"
              "library:\n"
              "  resources: []\n"
              "  tracks:\n"
              "    - uri: imported.flac\n"
              "      title: \""
           << title
           << "\"\n"
              "  lists: []\n";
    }

    template<typename T>
    auto spawnFuture(async::Runtime& runtime,
                     async::Task<T> task,
                     std::shared_ptr<std::atomic_bool> const& completedPtr)
    {
      return runtime.spawn(flagCompletion(completedPtr, std::move(task)));
    }

    bool isReady(std::shared_ptr<std::atomic_bool> const& completedPtr)
    {
      return completedPtr->load();
    }

    std::unique_ptr<Library> makeLibrary(async::Runtime& runtime,
                                         library::MusicLibrary& storage,
                                         LibraryChanges& changes)
    {
      return std::make_unique<Library>(runtime, ao::test::requireValue(Library::prepare(storage)), changes);
    }

    void requireBackgroundTaskLeaseReleased(async::Runtime& runtime, QueuedExecutor& executor, LibraryJobs& jobs)
    {
      auto completedPtr = std::make_shared<std::atomic_bool>(false);
      auto future = spawnFuture(runtime, jobs.backfillAudioIdentityAsync(), completedPtr);
      REQUIRE(executor.drainUntil([&completedPtr] { return isReady(completedPtr); }));
      REQUIRE(future.get());
    }
  } // namespace

  TEST_CASE("LibraryJobs - prepareLibraryImportAsync returns failure for invalid path", "[runtime][unit][library-task]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto executor = QueuedExecutor{};
    auto runtime = async::Runtime{executor};
    auto changes = makeLibraryChanges(executor, libraryFixture.library());
    auto runtimeLibraryPtr = makeLibrary(runtime, libraryFixture.library(), changes);
    auto& jobs = runtimeLibraryPtr->jobs();

    auto const result = runQueuedTask(
      runtime, executor, jobs.prepareLibraryImportAsync("/nonexistent_path_123.yaml", ImportMode::Restore));

    REQUIRE_FALSE(result);
    CHECK(result.error().code == Error::Code::IoError);
    CHECK(result.error().message.contains("Failed to read"));
    CHECK(std::string_view{result.error().location.file_name()}.contains("LibraryYamlImporter.cpp"));
  }

  TEST_CASE("LibraryJobs - import plans bind preview bytes and target state",
            "[runtime][unit][library-import][authorization]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const existingTrackId =
      libraryFixture.addTrack(library::test::TrackSpec{.title = "Existing", .uri = "existing.flac"});
    auto executor = QueuedExecutor{};
    auto runtime = async::Runtime{executor};
    auto changes = makeLibraryChanges(executor, libraryFixture.library());
    auto runtimeLibraryPtr = makeLibrary(runtime, libraryFixture.library(), changes);
    auto& jobs = runtimeLibraryPtr->jobs();
    auto const yamlPath = libraryFixture.root() / "import.yaml";
    writeImportPayload(yamlPath, "Prepared");
    auto planRes = runQueuedTask(runtime, executor, jobs.prepareLibraryImportAsync(yamlPath, ImportMode::Restore));

    REQUIRE(planRes);
    CHECK(planRes->report().payloadVersion == 5);
    CHECK(planRes->report().payloadMode == ExportMode::Full);
    CHECK(planRes->report().targetScope == ImportTargetScope::Library);
    CHECK(planRes->report().tracksCreated == 1);

    SECTION("unchanged preview applies")
    {
      auto result = runQueuedTask(runtime, executor, jobs.applyLibraryImportPlanAsync(std::move(*planRes)));

      INFO((result ? "import applied" : result.error().message));
      REQUIRE(result);
      CHECK(result->tracksCreated == 1);
    }

    SECTION("changed source is rejected")
    {
      writeImportPayload(yamlPath, "Changed");
      auto result = runQueuedTask(runtime, executor, jobs.applyLibraryImportPlanAsync(std::move(*planRes)));

      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::Conflict);
    }

    SECTION("changed target revision is rejected")
    {
      auto deleteRes = runQueuedTask(runtime, executor, runtimeLibraryPtr->commands().deleteTrack(existingTrackId));
      INFO((deleteRes ? "target changed" : deleteRes.error().message));
      REQUIRE(deleteRes);
      auto result = runQueuedTask(runtime, executor, jobs.applyLibraryImportPlanAsync(std::move(*planRes)));

      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::Conflict);
    }
  }

  TEST_CASE("LibraryJobs - import plans reject a different runtime over the same library",
            "[runtime][unit][library-import][authorization]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const yamlPath = libraryFixture.root() / "import.yaml";
    writeImportPayload(yamlPath, "Prepared");
    auto optPlan = std::optional<LibraryImportPlan>{};

    {
      auto executor = QueuedExecutor{};
      auto runtime = async::Runtime{executor};
      auto changes = makeLibraryChanges(executor, libraryFixture.library());
      auto runtimeLibraryPtr = makeLibrary(runtime, libraryFixture.library(), changes);
      auto result = runQueuedTask(
        runtime, executor, runtimeLibraryPtr->jobs().prepareLibraryImportAsync(yamlPath, ImportMode::Restore));

      REQUIRE(result);
      optPlan.emplace(std::move(*result));
    }

    auto otherExecutor = QueuedExecutor{};
    auto otherRuntime = async::Runtime{otherExecutor};
    auto otherChanges = makeLibraryChanges(otherExecutor, libraryFixture.library());
    auto otherLibraryPtr = makeLibrary(otherRuntime, libraryFixture.library(), otherChanges);
    auto result = runQueuedTask(
      otherRuntime, otherExecutor, otherLibraryPtr->jobs().applyLibraryImportPlanAsync(std::move(*optPlan)));

    REQUIRE_FALSE(result);
    CHECK(result.error().code == Error::Code::Conflict);
  }

  TEST_CASE("LibraryJobs - cancelled import preparation never enters maintenance",
            "[runtime][unit][library-import][concurrency]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto executor = QueuedExecutor{};
    auto runtime = async::Runtime{executor};
    auto changes = makeLibraryChanges(executor, libraryFixture.library());
    auto runtimeLibraryPtr = makeLibrary(runtime, libraryFixture.library(), changes);
    auto const yamlPath = libraryFixture.root() / "import.yaml";
    writeImportPayload(yamlPath, "Prepared");
    auto stopSource = std::stop_source{};

    SECTION("before start")
    {
      REQUIRE(stopSource.request_stop());
      auto future = runtime.spawn(
        runtimeLibraryPtr->jobs().prepareLibraryImportAsync(yamlPath, ImportMode::Restore, stopSource.get_token()));
      CHECK_THROWS_AS(std::ignore = future.get(), async::OperationCancelled);
    }

    SECTION("while callback admission is suspended")
    {
      auto completedPtr = std::make_shared<std::atomic_bool>(false);
      auto future = spawnFuture(
        runtime,
        runtimeLibraryPtr->jobs().prepareLibraryImportAsync(yamlPath, ImportMode::Restore, stopSource.get_token()),
        completedPtr);
      executor.checkQueued();

      REQUIRE(stopSource.request_stop());
      REQUIRE(executor.drainUntil([&completedPtr] { return isReady(completedPtr); }));
      CHECK_THROWS_AS(std::ignore = future.get(), async::OperationCancelled);
    }

    CHECK(runtimeLibraryPtr->authoringAvailability().state == LibraryAuthoringState::Available);
    runtime.requestStop();
    runtime.join();
  }

  TEST_CASE("LibraryJobs - import preview cancellation finishes maintenance on the callback owner",
            "[runtime][regression][library-import][concurrency]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto executor = QueuedExecutor{};
    auto runtime = async::Runtime{executor};
    auto changes = makeLibraryChanges(executor, libraryFixture.library());
    auto runtimeLibraryPtr = makeLibrary(runtime, libraryFixture.library(), changes);
    auto const yamlPath = libraryFixture.root() / "import.yaml";
    writeImportPayload(yamlPath, "Prepared");
    auto stopSource = std::stop_source{};
    auto observed = std::vector<LibraryAuthoringState>{};
    auto availabilitySubscription = runtimeLibraryPtr->onAuthoringAvailabilityChanged(
      [&](LibraryAuthoringAvailability const& availability) noexcept
      {
        observed.push_back(availability.state);

        if (availability.state == LibraryAuthoringState::Maintenance)
        {
          std::ignore = stopSource.request_stop();
        }
      });
    auto completedPtr = std::make_shared<std::atomic_bool>(false);
    auto future = spawnFuture(
      runtime,
      runtimeLibraryPtr->jobs().prepareLibraryImportAsync(yamlPath, ImportMode::Restore, stopSource.get_token()),
      completedPtr);

    REQUIRE(executor.drainUntil([&] { return isReady(completedPtr); }));
    CHECK_THROWS_AS(std::ignore = future.get(), async::OperationCancelled);
    CHECK(observed == std::vector{LibraryAuthoringState::Maintenance, LibraryAuthoringState::Available});
    CHECK(runtimeLibraryPtr->authoringAvailability().state == LibraryAuthoringState::Available);
    runtime.requestStop();
    runtime.join();
  }

  TEST_CASE("LibraryJobs - cancellation after import commit preserves mandatory completion",
            "[runtime][regression][library-import][concurrency]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto executor = QueuedExecutor{};
    auto runtime = async::Runtime{executor};
    auto changes = makeLibraryChanges(executor, libraryFixture.library());
    auto runtimeLibraryPtr = makeLibrary(runtime, libraryFixture.library(), changes);
    auto& jobs = runtimeLibraryPtr->jobs();
    auto const yamlPath = libraryFixture.root() / "import.yaml";
    writeImportPayload(yamlPath, "Committed");
    auto prepareCompletedPtr = std::make_shared<std::atomic_bool>(false);
    auto prepareFuture =
      spawnFuture(runtime, jobs.prepareLibraryImportAsync(yamlPath, ImportMode::Restore), prepareCompletedPtr);

    REQUIRE(executor.drainUntil([&prepareCompletedPtr] { return isReady(prepareCompletedPtr); }));
    auto planRes = prepareFuture.get();
    REQUIRE(planRes);
    executor.drain();
    REQUIRE(runtimeLibraryPtr->authoringAvailability().state == LibraryAuthoringState::Available);

    auto committed = AsyncTestState<bool>::create(false);
    auto changeSubscription = changes.onChanged([committed](LibraryChangeSet const&) noexcept { committed.set(true); });
    auto stopSource = std::stop_source{};
    auto applyCompletedPtr = std::make_shared<std::atomic_bool>(false);
    auto applyFuture = spawnFuture(
      runtime, jobs.applyLibraryImportPlanAsync(std::move(*planRes), stopSource.get_token()), applyCompletedPtr);

    REQUIRE(executor.drainUntil([&committed] { return committed.load(); }));
    REQUIRE(stopSource.request_stop());
    REQUIRE(executor.drainUntil([&applyCompletedPtr] { return isReady(applyCompletedPtr); }));
    auto result = applyFuture.get();

    REQUIRE(result);
    CHECK(result->tracksCreated == 1);
    executor.drain();
    CHECK(runtimeLibraryPtr->authoringAvailability().state == LibraryAuthoringState::Available);
    runtime.requestStop();
    runtime.join();
  }

  TEST_CASE("LibraryJobs - exportLibraryAsync returns failure for invalid path", "[runtime][unit][library-task]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto executor = QueuedExecutor{};
    auto runtime = async::Runtime{executor};
    auto changes = makeLibraryChanges(executor, libraryFixture.library());
    auto runtimeLibraryPtr = makeLibrary(runtime, libraryFixture.library(), changes);
    auto& jobs = runtimeLibraryPtr->jobs();

    auto const result =
      runQueuedTask(runtime, executor, jobs.exportLibraryAsync("/root/nonexistent_path_123.yaml", ExportMode::Full));

    REQUIRE_FALSE(result);
    CHECK(result.error().code == Error::Code::IoError);
  }

  TEST_CASE("LibraryJobs - YAML transfers publish coarse progress and one terminal pulse",
            "[runtime][unit][library-task][transfer]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto executor = QueuedExecutor{};
    auto runtime = async::Runtime{executor};
    auto changes = makeLibraryChanges(executor, libraryFixture.library());
    auto runtimeLibraryPtr = makeLibrary(runtime, libraryFixture.library(), changes);
    auto& jobs = runtimeLibraryPtr->jobs();
    auto events = std::vector<LibraryTaskProgressUpdated>{};
    auto finishedEvents = std::vector<LibraryTaskProgressFinished>{};
    auto progressSubscription =
      jobs.onProgress([&events](LibraryTaskProgressUpdated const& event) noexcept { events.push_back(event); });
    auto finishedSubscription = jobs.onProgressFinished(
      [&finishedEvents](LibraryTaskProgressFinished const& event) noexcept { finishedEvents.push_back(event); });
    auto const importPath = libraryFixture.root() / utility::pathFromUtf8("\xE5\xA4\x87\xE4\xBB\xBD.yaml");
    auto const exportPath = libraryFixture.root() / utility::pathFromUtf8("\xE5\xAF\xBC\xE5\x87\xBA.yaml");
    writeImportPayload(importPath, "Prepared");

    auto planRes = runQueuedTask(runtime, executor, jobs.prepareLibraryImportAsync(importPath, ImportMode::Merge));
    executor.drain();

    REQUIRE(planRes);
    REQUIRE(events.size() == 1);
    CHECK(events[0].kind == LibraryTaskProgressKind::PreparingImport);
    CHECK(events[0].subject == "\xE5\xA4\x87\xE4\xBB\xBD.yaml");
    CHECK(events[0].fraction == 0.0);
    REQUIRE(finishedEvents.size() == 1);
    CHECK(events[0].id != kInvalidLibraryTaskProgressId);
    CHECK(finishedEvents[0].id == events[0].id);
    auto const preparationId = events[0].id;

    events.clear();
    auto importRes = runQueuedTask(runtime, executor, jobs.applyLibraryImportPlanAsync(std::move(*planRes)));
    executor.drain();

    REQUIRE(importRes);
    REQUIRE(events.size() == 1);
    CHECK(events[0].kind == LibraryTaskProgressKind::Importing);
    CHECK(events[0].subject == "\xE5\xA4\x87\xE4\xBB\xBD.yaml");
    REQUIRE(finishedEvents.size() == 2);
    CHECK(events[0].id != preparationId);
    CHECK(finishedEvents[1].id == events[0].id);
    auto const importId = events[0].id;

    events.clear();
    auto exportRes = runQueuedTask(runtime, executor, jobs.exportLibraryAsync(exportPath, ExportMode::Full));
    executor.drain();

    REQUIRE(exportRes);
    REQUIRE(events.size() == 1);
    CHECK(events[0].kind == LibraryTaskProgressKind::Exporting);
    CHECK(events[0].subject == "\xE5\xAF\xBC\xE5\x87\xBA.yaml");
    REQUIRE(finishedEvents.size() == 3);
    CHECK(events[0].id != importId);
    CHECK(finishedEvents[2].id == events[0].id);
    CHECK(std::filesystem::exists(exportPath));
  }

  TEST_CASE("LibraryJobs - buildScanPlanAsync succeeds", "[runtime][unit][library-task][scan]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto executor = QueuedExecutor{};
    auto runtime = async::Runtime{executor};
    auto changes = makeLibraryChanges(executor, libraryFixture.library());
    auto runtimeLibraryPtr = makeLibrary(runtime, libraryFixture.library(), changes);
    auto& jobs = runtimeLibraryPtr->jobs();
    std::int32_t progressFinishedCount = 0;
    auto progressFinishedSub = jobs.onProgressFinished(
      [&progressFinishedCount](LibraryTaskProgressFinished const&) noexcept { ++progressFinishedCount; });

    auto const result = runQueuedTask(runtime, executor, jobs.buildScanPlanAsync());

    REQUIRE(result);
    CHECK(progressFinishedCount == 1);
  }

  TEST_CASE("LibraryJobs - pre-admission cancellation publishes no progress conversation",
            "[runtime][regression][library-task][concurrency]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto executor = QueuedExecutor{};
    auto runtime = async::Runtime{executor};
    auto changes = makeLibraryChanges(executor, libraryFixture.library());
    auto runtimeLibraryPtr = makeLibrary(runtime, libraryFixture.library(), changes);
    auto& jobs = runtimeLibraryPtr->jobs();
    std::int32_t progressCount = 0;
    std::int32_t progressFinishedCount = 0;
    std::int32_t availabilityCount = 0;
    auto progressSubscription =
      jobs.onProgress([&progressCount](LibraryTaskProgressUpdated const&) noexcept { ++progressCount; });
    auto progressFinishedSubscription = jobs.onProgressFinished(
      [&progressFinishedCount](LibraryTaskProgressFinished const&) noexcept { ++progressFinishedCount; });
    auto availabilitySubscription = runtimeLibraryPtr->onAuthoringAvailabilityChanged(
      [&availabilityCount](LibraryAuthoringAvailability const&) noexcept { ++availabilityCount; });
    auto stopSource = std::stop_source{};
    REQUIRE(stopSource.request_stop());
    auto completedPtr = std::make_shared<std::atomic_bool>(false);

    SECTION("scan plan build")
    {
      auto future = spawnFuture(runtime, jobs.buildScanPlanAsync(stopSource.get_token()), completedPtr);

      REQUIRE(executor.drainUntil([&completedPtr] { return isReady(completedPtr); }));
      CHECK(progressCount == 0);
      CHECK(progressFinishedCount == 0);
      CHECK(availabilityCount == 0);
      CHECK(runtimeLibraryPtr->authoringAvailability().state == LibraryAuthoringState::Available);
      requireCancellation(future, runtime);
    }

    SECTION("scan plan apply")
    {
      auto planRes = LibraryScan{libraryFixture.library()}.buildPlan();
      REQUIRE(planRes);
      auto future =
        spawnFuture(runtime, jobs.applyScanPlanAsync(std::move(*planRes), {}, stopSource.get_token()), completedPtr);

      REQUIRE(executor.drainUntil([&completedPtr] { return isReady(completedPtr); }));
      CHECK(progressCount == 0);
      CHECK(progressFinishedCount == 0);
      CHECK(availabilityCount == 0);
      CHECK(runtimeLibraryPtr->authoringAvailability().state == LibraryAuthoringState::Available);
      requireCancellation(future, runtime);
    }

    SECTION("audio identity backfill")
    {
      auto future = spawnFuture(runtime, jobs.backfillAudioIdentityAsync(stopSource.get_token()), completedPtr);

      REQUIRE(executor.drainUntil([&completedPtr] { return isReady(completedPtr); }));
      CHECK(progressCount == 0);
      CHECK(progressFinishedCount == 0);
      CHECK(availabilityCount == 0);
      CHECK(runtimeLibraryPtr->authoringAvailability().state == LibraryAuthoringState::Available);
      requireCancellation(future, runtime);
    }

    SECTION("import preview")
    {
      auto const yamlPath = libraryFixture.root() / "import.yaml";
      writeImportPayload(yamlPath, "Prepared");
      auto future = spawnFuture(
        runtime, jobs.prepareLibraryImportAsync(yamlPath, ImportMode::Merge, stopSource.get_token()), completedPtr);

      REQUIRE(executor.drainUntil([&completedPtr] { return isReady(completedPtr); }));
      CHECK(progressCount == 0);
      CHECK(progressFinishedCount == 0);
      CHECK(availabilityCount == 0);
      CHECK(runtimeLibraryPtr->authoringAvailability().state == LibraryAuthoringState::Available);
      requireCancellation(future, runtime);
    }

    SECTION("import apply")
    {
      auto const yamlPath = libraryFixture.root() / "import.yaml";
      writeImportPayload(yamlPath, "Prepared");
      auto planRes = runQueuedTask(runtime, executor, jobs.prepareLibraryImportAsync(yamlPath, ImportMode::Merge));
      REQUIRE(planRes);
      executor.drain();
      progressCount = 0;
      progressFinishedCount = 0;
      availabilityCount = 0;
      auto future = spawnFuture(
        runtime, jobs.applyLibraryImportPlanAsync(std::move(*planRes), stopSource.get_token()), completedPtr);

      REQUIRE(executor.drainUntil([&completedPtr] { return isReady(completedPtr); }));
      CHECK(progressCount == 0);
      CHECK(progressFinishedCount == 0);
      CHECK(availabilityCount == 0);
      CHECK(runtimeLibraryPtr->authoringAvailability().state == LibraryAuthoringState::Available);
      requireCancellation(future, runtime);
    }

    SECTION("export")
    {
      auto future = spawnFuture(
        runtime,
        jobs.exportLibraryAsync(libraryFixture.root() / "export.yaml", ExportMode::Full, stopSource.get_token()),
        completedPtr);

      REQUIRE(executor.drainUntil([&completedPtr] { return isReady(completedPtr); }));
      CHECK(progressCount == 0);
      CHECK(progressFinishedCount == 0);
      CHECK(availabilityCount == 0);
      CHECK(runtimeLibraryPtr->authoringAvailability().state == LibraryAuthoringState::Available);
      requireCancellation(future, runtime);
    }
  }

  TEST_CASE("LibraryJobs - scan plan preserves UTF-8 filenames", "[runtime][regression][library-task]")
  {
    auto const expected = std::string{"\xE8\xAA\xB0\xE3\x81\x8B\xE3\x80\x81\xE6\xB5\xB7\xE3\x82\x92\xE3\x80\x82.flac"};
    auto libraryFixture = MusicLibraryFixture{};
    std::filesystem::copy_file(
      audio::test::requireAudioFixture("basic_metadata.flac"), libraryFixture.root() / utility::pathFromUtf8(expected));
    auto executor = QueuedExecutor{};
    auto runtime = async::Runtime{executor};
    auto changes = makeLibraryChanges(executor, libraryFixture.library());
    auto runtimeLibraryPtr = makeLibrary(runtime, libraryFixture.library(), changes);
    auto& jobs = runtimeLibraryPtr->jobs();

    // Scanning progress is phase-coalesced, so a later path under the same
    // music root may replace this filename before callback delivery. The scan
    // plan is the durable per-file result and therefore owns this assertion.
    auto const result = runQueuedTask(runtime, executor, jobs.buildScanPlanAsync());

    REQUIRE(result);
    REQUIRE(result->size() == 1);
    CHECK(result->items().front().uri == expected);
  }

  TEST_CASE("LibraryJobs - applyScanPlanAsync succeeds with empty plan", "[runtime][unit][library-task][scan]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto executor = QueuedExecutor{};
    auto runtime = async::Runtime{executor};
    auto changes = makeLibraryChanges(executor, libraryFixture.library());
    auto observed = std::vector<LibraryChangeSet>{};
    auto changedSubscription =
      changes.onChanged([&observed](LibraryChangeSet const& changeSet) noexcept { observed.push_back(changeSet); });
    auto runtimeLibraryPtr = makeLibrary(runtime, libraryFixture.library(), changes);
    auto& jobs = runtimeLibraryPtr->jobs();

    auto plan = LibraryScan{libraryFixture.library()}.buildPlan().value();
    auto const result = runQueuedTask(runtime, executor, jobs.applyScanPlanAsync(std::move(plan)));

    REQUIRE(result);
    CHECK(result->insertedIds.empty());
    CHECK(result->mutatedIds.empty());
    CHECK(result->relinkedIds.empty());
    CHECK(result->failureCount == 0);
    CHECK(result->libraryRevision == 0);
    CHECK(observed.empty());
    auto transaction = libraryFixture.library().readTransaction();
    CHECK(libraryFixture.library().libraryRevision(transaction) == 0);
  }

  TEST_CASE("LibraryJobs - applyScanPlanAsync can defer new audio identity", "[runtime][unit][library-task]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const sourceFile = audio::test::requireAudioFixture("basic_metadata.flac");
    std::filesystem::copy_file(sourceFile, libraryFixture.root() / "song.flac");

    auto executor = QueuedExecutor{};
    auto runtime = async::Runtime{executor};
    auto changes = makeLibraryChanges(executor, libraryFixture.library());
    auto runtimeLibraryPtr = makeLibrary(runtime, libraryFixture.library(), changes);
    auto& jobs = runtimeLibraryPtr->jobs();

    auto scanService = LibraryScan{libraryFixture.library()};
    auto plan = scanService.buildPlan().value();
    REQUIRE(plan.count(ScanClassification::New) == 1);

    auto const result = runQueuedTask(
      runtime,
      executor,
      jobs.applyScanPlanAsync(std::move(plan), ScanApplyOptions{.audioIdentityPolicy = AudioIdentityPolicy::DeferNew}));

    REQUIRE(result);
    REQUIRE(result->insertedIds.size() == 1);
    auto transaction = libraryFixture.library().readTransaction();
    auto optManifest = libraryFixture.library().manifest().reader(transaction).get("song.flac");
    REQUIRE(optManifest);
    CHECK_FALSE(library::hasAudioIdentity(optManifest->audioPayloadLength(), optManifest->audioSignature()));
  }

  TEST_CASE("LibraryJobs - scan preparation permits unrelated interactive authoring",
            "[runtime][regression][library-task][concurrency]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const authoringTarget = libraryFixture.addTrack("Before");
    auto const sourceFile = audio::test::requireAudioFixture("basic_metadata.flac");
    std::filesystem::copy_file(sourceFile, libraryFixture.root() / "song.flac");
    auto scanService = LibraryScan{libraryFixture.library()};
    auto planRes = scanService.buildPlan();
    REQUIRE(planRes);

    auto executor = QueuedExecutor{};
    auto runtime = async::Runtime{executor};
    auto changes = makeLibraryChanges(executor, libraryFixture.library());
    auto runtimeLibraryPtr = makeLibrary(runtime, libraryFixture.library(), changes);
    auto bindingRes = runtimeLibraryPtr->bindTrackTargets(std::array{authoringTarget});
    REQUIRE(bindingRes);
    auto preparationStarted = AsyncTestState<bool>::create(false);
    auto releasePreparation = AsyncBarrier{};
    auto completedPtr = std::make_shared<std::atomic_bool>(false);
    auto future = spawnFuture(
      runtime,
      runtimeLibraryPtr->jobs().applyScanPlanAsync(std::move(*planRes),
                                                   {},
                                                   {},
                                                   [&preparationStarted, &releasePreparation](ScanApplyProgress const&)
                                                   {
                                                     if (!preparationStarted.load())
                                                     {
                                                       preparationStarted.set(true);
                                                       releasePreparation.wait();
                                                     }
                                                   }),
      completedPtr);

    auto const startedInTime = executor.drainUntil([&preparationStarted] { return preparationStarted.load(); });

    if (startedInTime)
    {
      auto const availability = runtimeLibraryPtr->authoringAvailability();
      CHECK(availability.state == LibraryAuthoringState::Available);

      auto authoringRes = runQueuedTask(
        runtime,
        executor,
        runtimeLibraryPtr->commands().updateMetadata(*bindingRes, MetadataPatch{.optTitle = "Edited during scan"}));
      CHECK(authoringRes);

      if (authoringRes)
      {
        CHECK(authoringRes->status == AuthoringStatus::Applied);
      }

      executor.drain();

      auto listRes = runQueuedTask(
        runtime, executor, runtimeLibraryPtr->commands().createList(ListDraft{.name = "Created during scan"}));
      CHECK(listRes);

      auto overlapCompletedPtr = std::make_shared<std::atomic_bool>(false);
      auto overlapFuture =
        spawnFuture(runtime, runtimeLibraryPtr->jobs().backfillAudioIdentityAsync(), overlapCompletedPtr);
      auto const overlapCompleted =
        executor.drainUntil([&overlapCompletedPtr] { return isReady(overlapCompletedPtr); });
      CHECK(overlapCompleted);

      if (overlapCompleted)
      {
        auto overlapRes = overlapFuture.get();
        CHECK_FALSE(overlapRes);

        if (!overlapRes)
        {
          CHECK(overlapRes.error().code == Error::Code::ResourceBusy);
        }
      }
    }

    releasePreparation.release();
    REQUIRE(startedInTime);
    REQUIRE(executor.drainUntil([&completedPtr] { return isReady(completedPtr); }));
    auto result = future.get();
    REQUIRE(result);
    REQUIRE(result->insertedIds.size() == 1);
    CHECK(runtimeLibraryPtr->authoringAvailability().state == LibraryAuthoringState::Available);
    {
      auto transaction = libraryFixture.library().readTransaction();
      auto const optTrack = libraryFixture.library().tracks().reader(transaction).get(authoringTarget);
      REQUIRE(optTrack);
      CHECK(optTrack->metadata().title() == "Edited during scan");
    }

    auto postScanBindingRes = runtimeLibraryPtr->bindTrackTargets(std::array{authoringTarget});
    REQUIRE(postScanBindingRes);
    auto postScanAuthoringRes = runQueuedTask(runtime,
                                              executor,
                                              runtimeLibraryPtr->commands().updateMetadata(
                                                *postScanBindingRes, MetadataPatch{.optTitle = "Edited after scan"}));
    REQUIRE(postScanAuthoringRes);
    CHECK(postScanAuthoringRes->status == AuthoringStatus::Applied);
    runtime.requestStop();
    runtime.join();
  }

  TEST_CASE("LibraryJobs - backfillAudioIdentityAsync fills pending rows", "[runtime][unit][library-task]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const sourceFile = audio::test::requireAudioFixture("basic_metadata.flac");
    std::filesystem::copy_file(sourceFile, libraryFixture.root() / "song.flac");

    auto executor = QueuedExecutor{};
    auto runtime = async::Runtime{executor};
    auto changes = makeLibraryChanges(executor, libraryFixture.library());
    auto runtimeLibraryPtr = makeLibrary(runtime, libraryFixture.library(), changes);
    auto& jobs = runtimeLibraryPtr->jobs();

    auto scanService = LibraryScan{libraryFixture.library()};
    auto plan = scanService.buildPlan().value();
    auto const applyRes = runQueuedTask(
      runtime,
      executor,
      jobs.applyScanPlanAsync(std::move(plan), ScanApplyOptions{.audioIdentityPolicy = AudioIdentityPolicy::DeferNew}));
    REQUIRE(applyRes);

    auto const backfillRes = runQueuedTask(runtime, executor, jobs.backfillAudioIdentityAsync());

    REQUIRE(backfillRes);
    CHECK(backfillRes->completedCount == 1);
    CHECK(backfillRes->skippedCount == 0);
    CHECK(backfillRes->failureCount == 0);

    auto transaction = libraryFixture.library().readTransaction();
    auto optManifest = libraryFixture.library().manifest().reader(transaction).get("song.flac");
    REQUIRE(optManifest);
    CHECK(library::hasAudioIdentity(optManifest->audioPayloadLength(), optManifest->audioSignature()));
  }

  TEST_CASE("LibraryJobs - applyScanPlanAsync reports progress while applying plan",
            "[runtime][unit][library-task][scan]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const sourceFile = audio::test::requireAudioFixture("basic_metadata.flac");
    auto const firstFile =
      libraryFixture.root() /
      utility::pathFromUtf8("\xE8\xAA\xB0\xE3\x81\x8B\xE3\x80\x81\xE6\xB5\xB7\xE3\x82\x92\xE3\x80\x82.flac");
    auto const secondFile = libraryFixture.root() / "second.flac";
    std::filesystem::copy_file(sourceFile, firstFile);
    std::filesystem::copy_file(sourceFile, secondFile);
    auto executor = QueuedExecutor{};
    auto runtime = async::Runtime{executor};
    auto changes = makeLibraryChanges(executor, libraryFixture.library());
    auto runtimeLibraryPtr = makeLibrary(runtime, libraryFixture.library(), changes);
    auto& jobs = runtimeLibraryPtr->jobs();

    auto progressEvents = std::vector<LibraryTaskProgressUpdated>{};
    auto sub = runtimeLibraryPtr->jobs().onProgress([&](auto const& ev) noexcept { progressEvents.push_back(ev); });
    auto plan = LibraryScan{libraryFixture.library()}.buildPlan().value();
    auto expectedNames = std::vector<std::string>{};
    std::int32_t failureCallbackCount = 0;

    for (auto const& item : plan.items())
    {
      expectedNames.push_back(utility::pathToUtf8(item.fullPath.filename()));
    }

    std::filesystem::remove(firstFile);
    std::filesystem::remove(secondFile);

    auto const result = runQueuedTask(
      runtime,
      executor,
      jobs.applyScanPlanAsync(
        std::move(plan), {}, {}, {}, [&failureCallbackCount](ScanFailure const&) { ++failureCallbackCount; }));

    REQUIRE(result);
    CHECK(result->insertedIds.empty());
    CHECK(result->mutatedIds.empty());
    CHECK(result->relinkedIds.empty());
    CHECK(result->failureCount == 2);
    CHECK(failureCallbackCount == 2);

    REQUIRE(expectedNames.size() == 2);
    REQUIRE_FALSE(progressEvents.empty());
    CHECK(progressEvents.size() <= expectedNames.size());
    CHECK(progressEvents.back().kind == LibraryTaskProgressKind::Updating);
    CHECK(progressEvents.back().subject == expectedNames.back());
    CHECK(progressEvents.back().fraction == 0.5);

    for (auto const& event : progressEvents)
    {
      CHECK(event.fraction >= 0.0);
      CHECK(event.fraction <= 1.0);
      CHECK_FALSE(event.subject.empty());
    }
  }

  TEST_CASE("LibraryJobs - scan progress coalesces while callback delivery is backlogged",
            "[runtime][regression][library-task][concurrency]")
  {
    constexpr std::size_t kFileCount = 64;
    auto libraryFixture = MusicLibraryFixture{};
    auto const sourceFile = audio::test::requireAudioFixture("basic_metadata.flac");

    for (std::size_t index = 0; index < kFileCount; ++index)
    {
      std::filesystem::copy_file(sourceFile, libraryFixture.root() / std::format("track-{:02}.flac", index));
    }

    auto executor = QueuedExecutor{};
    auto runtime = async::Runtime{executor};
    auto changes = makeLibraryChanges(executor, libraryFixture.library());
    auto runtimeLibraryPtr = makeLibrary(runtime, libraryFixture.library(), changes);
    auto& jobs = runtimeLibraryPtr->jobs();
    auto plan = LibraryScan{libraryFixture.library()}.buildPlan().value();
    REQUIRE(plan.size() == kFileCount);
    auto const expectedLastSubject = utility::pathToUtf8(plan.items().back().fullPath.filename());

    for (auto const& item : plan.items())
    {
      std::filesystem::remove(item.fullPath);
    }

    auto progressEvents = std::vector<LibraryTaskProgressUpdated>{};
    auto eventOrder = std::vector<std::string>{};
    auto const progressSubscription = jobs.onProgress(
      [&progressEvents, &eventOrder](LibraryTaskProgressUpdated const& event)
      {
        progressEvents.push_back(event);
        eventOrder.emplace_back("progress");
      });
    auto const finishedSubscription = jobs.onProgressFinished([&eventOrder](LibraryTaskProgressFinished const&)
                                                              { eventOrder.emplace_back("finished"); });
    auto firstProgressEntered = AsyncTestState<bool>::create(false);
    auto progressCount = AsyncTestState<std::size_t>::create(0);
    auto firstProgressRelease = AsyncBarrier{};
    auto completedPtr = std::make_shared<std::atomic_bool>(false);
    auto future = spawnFuture(
      runtime,
      jobs.applyScanPlanAsync(std::move(plan),
                              {},
                              {},
                              [firstProgressEntered, progressCount, &firstProgressRelease](ScanApplyProgress const&)
                              {
                                if (progressCount.increment() == 1)
                                {
                                  firstProgressEntered.set(true);
                                  firstProgressRelease.wait();
                                }
                              }),
      completedPtr);

    REQUIRE(executor.drainUntil([firstProgressEntered] { return firstProgressEntered.load(); }));
    firstProgressRelease.release();
    REQUIRE(progressCount.waitUntil(kFileCount));

    // One progress delivery may already have run before the worker barrier;
    // the remaining burst owns at most one queued delivery plus its completion.
    CHECK(executor.queuedCount() <= 2);
    REQUIRE(executor.drainUntil([&completedPtr] { return isReady(completedPtr); }));
    auto const result = future.get();
    executor.drain();

    REQUIRE(result);
    CHECK(std::cmp_equal(result->failureCount, kFileCount));
    REQUIRE_FALSE(progressEvents.empty());
    CHECK(progressEvents.size() <= 2);
    CHECK(progressEvents.back().subject == expectedLastSubject);
    CHECK(progressEvents.back().fraction == static_cast<double>(kFileCount - 1) / static_cast<double>(kFileCount));
    REQUIRE_FALSE(eventOrder.empty());
    CHECK(eventOrder.back() == "finished");

    runtime.requestStop();
    runtime.join();
  }

  TEST_CASE("LibraryJobs - applyScanPlanAsync forwards cancellation to scan executor",
            "[runtime][unit][library-task][scan]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const sourceFile = audio::test::requireAudioFixture("basic_metadata.flac");
    std::filesystem::copy_file(sourceFile, libraryFixture.root() / "song.flac");

    auto executor = QueuedExecutor{};
    auto runtime = async::Runtime{executor};
    auto changes = makeLibraryChanges(executor, libraryFixture.library());
    auto observed = std::vector<LibraryChangeSet>{};
    auto changedSubscription =
      changes.onChanged([&observed](LibraryChangeSet const& changeSet) noexcept { observed.push_back(changeSet); });
    auto runtimeLibraryPtr = makeLibrary(runtime, libraryFixture.library(), changes);
    auto& jobs = runtimeLibraryPtr->jobs();

    auto scanService = LibraryScan{libraryFixture.library()};
    auto plan = scanService.buildPlan().value();
    REQUIRE(plan.count(ScanClassification::New) == 1);

    auto fingerprintingEntered = AsyncTestState<bool>::create(false);
    auto fingerprintingRelease = AsyncBarrier{};
    auto sawCancellation = AsyncTestState<bool>::create(false);
    auto progressFinished = AsyncTestState<bool>::create(false);
    auto progressFinishedSub = runtimeLibraryPtr->jobs().onProgressFinished(
      [progressFinished](LibraryTaskProgressFinished const&) noexcept { progressFinished.set(true); });

    auto taskHandle = runtime.spawnCancellable(
      [jobs = &jobs,
       plan = std::move(plan),
       fingerprintingEntered,
       fingerprintingBarrier = &fingerprintingRelease,
       sawCancellation](std::stop_token const stopToken) mutable
      {
        return applyScanPlanAndRecordCancellation(
          jobs, std::move(plan), fingerprintingEntered, fingerprintingBarrier, sawCancellation, stopToken);
      });

    REQUIRE(executor.drainUntil([&fingerprintingEntered] { return fingerprintingEntered.load(); }));
    taskHandle.reset();
    fingerprintingRelease.release();
    REQUIRE(executor.drainUntil([&sawCancellation] { return sawCancellation.load(); }));
    CHECK(fingerprintingEntered.load());
    CHECK(progressFinished.load());

    auto transaction = libraryFixture.library().readTransaction();
    auto trackReader = libraryFixture.library().tracks().reader(transaction);
    auto manifestReader = libraryFixture.library().manifest().reader(transaction);
    CHECK(trackReader.begin() == trackReader.end());
    CHECK(manifestReader.begin() == manifestReader.end());
    CHECK(libraryFixture.library().libraryRevision(transaction) == 0);
    CHECK(observed.empty());
    CHECK(runtimeLibraryPtr->authoringAvailability().state == LibraryAuthoringState::Available);
    requireBackgroundTaskLeaseReleased(runtime, executor, jobs);

    runtime.requestStop();
    runtime.join();
  }

  TEST_CASE("LibraryJobs - throwing scan progress callback propagates after background lease cleanup",
            "[runtime][regression][library-task][concurrency]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto executor = QueuedExecutor{};
    auto runtime = async::Runtime{executor};
    auto changes = makeLibraryChanges(executor, libraryFixture.library());
    auto runtimeLibraryPtr = makeLibrary(runtime, libraryFixture.library(), changes);
    auto& jobs = runtimeLibraryPtr->jobs();
    auto const sourceFile = audio::test::requireAudioFixture("basic_metadata.flac");
    auto const targetFile = libraryFixture.root() / "song.flac";
    std::filesystem::copy_file(sourceFile, targetFile);
    auto plan = LibraryScan{libraryFixture.library()}.buildPlan().value();
    std::int32_t callbackCount = 0;
    auto completedPtr = std::make_shared<std::atomic_bool>(false);
    auto future =
      spawnFuture(runtime,
                  jobs.applyScanPlanAsync(std::move(plan),
                                          {},
                                          {},
                                          [&callbackCount](ScanApplyProgress const&)
                                          {
                                            ++callbackCount;
                                            throw std::runtime_error{"injected library task callback failure"};
                                          }),
                  completedPtr);

    REQUIRE(executor.drainUntil([&] { return isReady(completedPtr); }));
    CHECK_THROWS_AS(future.get(), std::runtime_error);
    CHECK(callbackCount > 0);
    CHECK(runtimeLibraryPtr->authoringAvailability().state == LibraryAuthoringState::Available);
    requireBackgroundTaskLeaseReleased(runtime, executor, jobs);
    runtime.requestStop();
    runtime.join();
  }

  TEST_CASE("LibraryJobs - throwing backfill progress callback propagates after background lease cleanup",
            "[runtime][regression][library-task][concurrency]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const sourceFile = audio::test::requireAudioFixture("basic_metadata.flac");
    std::filesystem::copy_file(sourceFile, libraryFixture.root() / "song.flac");
    auto scanService = LibraryScan{libraryFixture.library()};
    auto planRes = scanService.buildPlan();
    REQUIRE(planRes);
    auto applyRes = ScanApplyOperation{libraryFixture.library(),
                                       std::move(*planRes),
                                       {},
                                       {},
                                       ScanApplyOptions{.audioIdentityPolicy = AudioIdentityPolicy::DeferNew}}
                      .run();
    REQUIRE(applyRes);
    REQUIRE(applyRes->insertedIds.size() == 1);

    auto executor = QueuedExecutor{};
    auto runtime = async::Runtime{executor};
    auto changes = makeLibraryChanges(executor, libraryFixture.library());
    auto runtimeLibraryPtr = makeLibrary(runtime, libraryFixture.library(), changes);
    auto& jobs = runtimeLibraryPtr->jobs();
    std::int32_t callbackCount = 0;
    auto completedPtr = std::make_shared<std::atomic_bool>(false);
    auto future =
      spawnFuture(runtime,
                  jobs.backfillAudioIdentityAsync({},
                                                  [&callbackCount](AudioIdentityIndexProgress const&)
                                                  {
                                                    ++callbackCount;
                                                    throw std::runtime_error{"injected library task callback failure"};
                                                  }),
                  completedPtr);

    REQUIRE(executor.drainUntil([&] { return isReady(completedPtr); }));
    CHECK_THROWS_AS(future.get(), std::runtime_error);
    CHECK(callbackCount > 0);
    CHECK(runtimeLibraryPtr->authoringAvailability().state == LibraryAuthoringState::Available);
    requireBackgroundTaskLeaseReleased(runtime, executor, jobs);
    runtime.requestStop();
    runtime.join();
  }

  TEST_CASE("LibraryJobs - apply cancellation finishes the background lease before propagation",
            "[runtime][regression][library-task][concurrency]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto executor = QueuedExecutor{};
    auto runtime = async::Runtime{executor};
    auto changes = makeLibraryChanges(executor, libraryFixture.library());
    auto runtimeLibraryPtr = makeLibrary(runtime, libraryFixture.library(), changes);
    auto& jobs = runtimeLibraryPtr->jobs();
    auto const sourceFile = audio::test::requireAudioFixture("basic_metadata.flac");
    auto const targetFile = libraryFixture.root() / "song.flac";
    std::filesystem::copy_file(sourceFile, targetFile);
    auto plan = LibraryScan{libraryFixture.library()}.buildPlan().value();
    auto stopSource = std::stop_source{};
    auto progressFinished = AsyncTestState<bool>::create(false);
    auto progressFinishedSub = jobs.onProgressFinished([progressFinished](LibraryTaskProgressFinished const&) noexcept
                                                       { progressFinished.set(true); });
    auto completedPtr = std::make_shared<std::atomic_bool>(false);
    auto future = spawnFuture(
      runtime,
      jobs.applyScanPlanAsync(std::move(plan),
                              {},
                              stopSource.get_token(),
                              [&stopSource](ScanApplyProgress const&) { std::ignore = stopSource.request_stop(); }),
      completedPtr);

    REQUIRE(executor.drainUntil([&] { return isReady(completedPtr); }));
    CHECK(runtimeLibraryPtr->authoringAvailability().state == LibraryAuthoringState::Available);
    CHECK(progressFinished.load());
    CHECK_THROWS_AS(future.get(), async::OperationCancelled);
    requireBackgroundTaskLeaseReleased(runtime, executor, jobs);
    runtime.requestStop();
    runtime.join();
  }

  TEST_CASE("LibraryJobs - backfill cancellation finishes the background lease before propagation",
            "[runtime][regression][library-task][concurrency]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const sourceFile = audio::test::requireAudioFixture("basic_metadata.flac");
    std::filesystem::copy_file(sourceFile, libraryFixture.root() / "song.flac");
    auto planRes = LibraryScan{libraryFixture.library()}.buildPlan();
    REQUIRE(planRes);
    auto applyRes = ScanApplyOperation{libraryFixture.library(),
                                       std::move(*planRes),
                                       {},
                                       {},
                                       ScanApplyOptions{.audioIdentityPolicy = AudioIdentityPolicy::DeferNew}}
                      .run();
    REQUIRE(applyRes);
    REQUIRE(applyRes->insertedIds.size() == 1);

    auto executor = QueuedExecutor{};
    auto runtime = async::Runtime{executor};
    auto changes = makeLibraryChanges(executor, libraryFixture.library());
    auto runtimeLibraryPtr = makeLibrary(runtime, libraryFixture.library(), changes);
    auto& jobs = runtimeLibraryPtr->jobs();
    auto stopSource = std::stop_source{};
    auto progressFinished = AsyncTestState<bool>::create(false);
    auto progressFinishedSub = jobs.onProgressFinished([progressFinished](LibraryTaskProgressFinished const&) noexcept
                                                       { progressFinished.set(true); });
    auto completedPtr = std::make_shared<std::atomic_bool>(false);
    auto future = spawnFuture(runtime,
                              jobs.backfillAudioIdentityAsync(stopSource.get_token(),
                                                              [&stopSource](AudioIdentityIndexProgress const&)
                                                              { std::ignore = stopSource.request_stop(); }),
                              completedPtr);

    REQUIRE(executor.drainUntil([&] { return isReady(completedPtr); }));
    CHECK(runtimeLibraryPtr->authoringAvailability().state == LibraryAuthoringState::Available);
    CHECK(progressFinished.load());
    CHECK_THROWS_AS(future.get(), async::OperationCancelled);
    requireBackgroundTaskLeaseReleased(runtime, executor, jobs);
    runtime.requestStop();
    runtime.join();
  }
} // namespace ao::rt::test
