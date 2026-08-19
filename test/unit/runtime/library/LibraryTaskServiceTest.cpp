// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/library/LibraryTaskService.h>

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
#include <ao/async/Executor.h>
#include <ao/async/OperationCancelled.h>
#include <ao/async/Runtime.h>
#include <ao/async/Task.h>
#include <ao/async/TaskFuture.h>
#include <ao/library/AudioIdentity.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/ResourceStore.h>
#include <ao/library/TrackStore.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/library/AudioIdentityIndex.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryScan.h>
#include <ao/rt/library/LibraryTaskEvents.h>
#include <ao/rt/library/LibraryWriter.h>
#include <ao/rt/library/LibraryYamlExporter.h>
#include <ao/rt/library/LibraryYamlImporter.h>
#include <ao/rt/library/ScanPlan.h>
#include <ao/rt/resource/ResourceDiskCache.h>
#include <ao/utility/Path.h>
#include <ao/utility/Sha256.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <span>
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

    async::Task<void> applyScanPlanAndRecordCancellation(LibraryTaskService* service,
                                                         ScanPlan plan,
                                                         AsyncTestState<bool> fingerprintingEntered,
                                                         AsyncBarrier* fingerprintingRelease,
                                                         AsyncTestState<bool> sawCancellation,
                                                         std::stop_token const stopToken)
    {
      try
      {
        [[maybe_unused]] auto result = co_await service->applyScanPlanAsync(
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

    ResourceId writeResource(library::MusicLibrary& library, std::span<std::byte const> bytes)
    {
      auto transaction = library::test::writeTransaction(library);
      auto result = library::test::physicalWriter(library.resources(), transaction).create(bytes);
      REQUIRE(result);
      REQUIRE(transaction.commit());
      return *result;
    }

    /**
     * @brief Installs @p bytes in the derived cache the runtime will consult.
     *
     * Written through the production cache rather than by hand, so the test never
     * restates the entry layout, and asserts the walk against the same tier a
     * real session fills.
     */
    void installCacheEntry(std::filesystem::path const& cacheRoot, std::span<std::byte const> bytes)
    {
      auto const cache = ResourceDiskCache{ResourceDiskCache::Config{
        .directory = coverCacheDirectory(cacheRoot),
        .maximumEntryBytes = LibraryTaskService::kMaximumInteractiveResourceBytes,
      }};
      cache.store(utility::computeSha256(bytes), bytes);
    }

    async::Task<bool> loadResourceAndCheckExecutor(LibraryTaskService* service,
                                                   async::Executor* executor,
                                                   ResourceId resourceId)
    {
      auto result = co_await service->loadResourceAsync(resourceId);
      REQUIRE(result);
      REQUIRE(*result);
      co_return executor->isCurrent();
    }

    async::Task<Result<std::optional<std::vector<std::byte>>>> loadResource(LibraryTaskService* service,
                                                                            ResourceId resourceId,
                                                                            ResourceSizeLimit limit)
    {
      co_return co_await service->loadResourceAsync(resourceId, limit);
    }

    template<typename T>
    async::Task<T> countCompletion(std::shared_ptr<std::atomic<std::size_t>> counterPtr, async::Task<T> task)
    {
      auto valueRes = co_await std::move(task);
      counterPtr->fetch_add(1);
      co_return valueRes;
    }
  } // namespace

  TEST_CASE("LibraryTaskService - interactive resource reads return owned bytes on the callback executor",
            "[runtime][unit][library-task][concurrency]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const bytes = std::array{std::byte{0x10}, std::byte{0x20}, std::byte{0x30}};
    auto const resourceId = writeResource(libraryFixture.library(), bytes);
    auto const cacheRoot = libraryFixture.root() / "cache";
    installCacheEntry(cacheRoot, bytes);
    auto executor = QueuedExecutor{};
    auto runtime = async::Runtime{executor};
    auto changes = makeLibraryChanges(executor, libraryFixture.library());
    auto runtimeLibraryPtr =
      ao::test::requireValue(Library::create(runtime, libraryFixture.library(), changes, cacheRoot));
    std::int32_t progressFinishedCount = 0;
    auto progressFinishedSub = runtimeLibraryPtr->taskService().onProgressFinished([&progressFinishedCount] noexcept
                                                                                   { ++progressFinishedCount; });
    auto completedPtr = std::make_shared<std::atomic_bool>(false);
    auto future = spawnFuture(
      runtime, loadResourceAndCheckExecutor(&runtimeLibraryPtr->taskService(), &executor, resourceId), completedPtr);

    REQUIRE(executor.drainUntil([&completedPtr] { return isReady(completedPtr); }));
    CHECK(future.get());
    CHECK(progressFinishedCount == 0);

    auto missingCompletedPtr = std::make_shared<std::atomic_bool>(false);
    auto missingFuture =
      spawnFuture(runtime, runtimeLibraryPtr->taskService().loadResourceAsync(ResourceId{987654}), missingCompletedPtr);
    REQUIRE(executor.drainUntil([&missingCompletedPtr] { return isReady(missingCompletedPtr); }));
    auto missingRes = missingFuture.get();
    REQUIRE(missingRes);
    CHECK_FALSE(*missingRes);

    auto invalidCompletedPtr = std::make_shared<std::atomic_bool>(false);
    auto invalidFuture =
      spawnFuture(runtime, runtimeLibraryPtr->taskService().loadResourceAsync(kInvalidResourceId), invalidCompletedPtr);
    REQUIRE(executor.drainUntil([&invalidCompletedPtr] { return isReady(invalidCompletedPtr); }));
    auto invalidRes = invalidFuture.get();
    REQUIRE(invalidRes);
    CHECK_FALSE(*invalidRes);

    runtime.requestStop();
    runtime.join();
  }

  TEST_CASE("LibraryTaskService - interactive resource encoded-byte limit is exact", "[runtime][unit][library-task]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto executor = QueuedExecutor{};
    auto runtime = async::Runtime{executor};
    auto changes = makeLibraryChanges(executor, libraryFixture.library());
    auto runtimeLibraryPtr = std::unique_ptr<Library>{};

    auto const cacheRoot = libraryFixture.root() / "cache";

    SECTION("materialized bytes at the limit are returned")
    {
      auto bytes = std::vector<std::byte>(LibraryTaskService::kMaximumInteractiveResourceBytes, std::byte{0x4A});
      auto const resourceId = writeResource(libraryFixture.library(), bytes);
      installCacheEntry(cacheRoot, bytes);
      runtimeLibraryPtr =
        ao::test::requireValue(Library::create(runtime, libraryFixture.library(), changes, cacheRoot));
      auto result = runQueuedTask(runtime, executor, runtimeLibraryPtr->taskService().loadResourceAsync(resourceId));

      REQUIRE(result);
      REQUIRE(*result);
      CHECK((*result)->size() == LibraryTaskService::kMaximumInteractiveResourceBytes);
      CHECK((*result)->front() == std::byte{0x4A});
      CHECK((*result)->back() == std::byte{0x4A});
    }

    SECTION("materialized bytes above the limit are rejected before publication")
    {
      auto bytes = std::vector<std::byte>(LibraryTaskService::kMaximumInteractiveResourceBytes + 1, std::byte{0x5B});
      auto const resourceId = writeResource(libraryFixture.library(), bytes);

      // The cache refuses an entry no frontend may serve, so this one is placed
      // with a cache configured for the administrative case: the point under test
      // is that the interactive request refuses the bytes it materialized.
      auto const oversizedCache = ResourceDiskCache{ResourceDiskCache::Config{
        .directory = coverCacheDirectory(cacheRoot),
        .maximumEntryBytes = bytes.size(),
      }};
      oversizedCache.store(utility::computeSha256(bytes), bytes);
      runtimeLibraryPtr =
        ao::test::requireValue(Library::create(runtime, libraryFixture.library(), changes, cacheRoot));
      auto result = runQueuedTask(runtime, executor, runtimeLibraryPtr->taskService().loadResourceAsync(resourceId));

      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::ValueTooLarge);

      // Administrative export keeps the exemption the delivery specification
      // grants it, over the same walk.
      auto administrativeRes =
        runQueuedTask(runtime,
                      executor,
                      loadResource(&runtimeLibraryPtr->taskService(), resourceId, ResourceSizeLimit::Administrative));
      REQUIRE(administrativeRes);
      REQUIRE(*administrativeRes);
      CHECK((*administrativeRes)->size() == bytes.size());
    }

    SECTION("a descriptor with no cache entry and no carrier yields no image")
    {
      auto const bytes = std::array{std::byte{0x01}, std::byte{0x02}, std::byte{0x03}};
      auto const resourceId = writeResource(libraryFixture.library(), bytes);
      runtimeLibraryPtr =
        ao::test::requireValue(Library::create(runtime, libraryFixture.library(), changes, cacheRoot));
      auto result = runQueuedTask(runtime, executor, runtimeLibraryPtr->taskService().loadResourceAsync(resourceId));

      REQUIRE(result);
      CHECK_FALSE(*result);
    }
  }

  TEST_CASE("LibraryTaskService - the carrier index is built lazily and rebuilt once per revision",
            "[runtime][unit][library-task][resource-walk]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const bytes = std::array{std::byte{0xA1}, std::byte{0xB2}};
    auto const resourceId = writeResource(libraryFixture.library(), bytes);
    auto executor = QueuedExecutor{};
    auto runtime = async::Runtime{executor};
    auto changes = makeLibraryChanges(executor, libraryFixture.library());
    auto runtimeLibraryPtr = ao::test::requireValue(
      Library::create(runtime, libraryFixture.library(), changes, libraryFixture.root() / "cache"));
    auto& service = runtimeLibraryPtr->taskService();

    // Laziness is what removes the ordering problem rather than deferring it:
    // there is nothing to publish ahead of a revision.
    CHECK(service.resourceCarrierIndexBuildCount() == 0);

    REQUIRE(runQueuedTask(runtime, executor, service.loadResourceAsync(resourceId)));
    CHECK(service.resourceCarrierIndexBuildCount() == 1);

    SECTION("a second request at the same revision reuses the snapshot")
    {
      REQUIRE(runQueuedTask(runtime, executor, service.loadResourceAsync(resourceId)));
      CHECK(service.resourceCarrierIndexBuildCount() == 1);
    }

    SECTION("a request after the revision moves sees a new snapshot")
    {
      // The runtime holds the writer session, so the revision has to move through
      // it; a stale stamp is all the next miss needs to rebuild.
      REQUIRE(runtimeLibraryPtr->createList(LibraryWriter::ListDraft{.name = "Revision bump"}));
      executor.drain();
      REQUIRE(runQueuedTask(runtime, executor, service.loadResourceAsync(resourceId)));
      CHECK(service.resourceCarrierIndexBuildCount() == 2);
    }

    runtime.requestStop();
    runtime.join();
  }

  TEST_CASE("LibraryTaskService - one stale stamp costs one index build across several workers",
            "[runtime][unit][library-task][concurrency]")
  {
    constexpr std::size_t kRequestCount = 50;
    auto libraryFixture = MusicLibraryFixture{};
    auto const bytes = std::array{std::byte{0xC3}, std::byte{0xD4}};
    auto const resourceId = writeResource(libraryFixture.library(), bytes);
    auto executor = QueuedExecutor{};

    // What this observes is the multiple-worker row of the concurrency matrix:
    // the one-build contract holds with four workers rather than one. It is not
    // a proof that the rebuild mutex serializes a simultaneous burst, because
    // nothing here forces two requests to be inside the stale check at once, and
    // no test can force that without a synchronization point inside the build.
    // Requests that happen to serialize satisfy the assertion the same way, by
    // finding the published snapshot at the stale check instead of at the mutex.
    auto runtime = async::Runtime{executor, 4};
    auto changes = makeLibraryChanges(executor, libraryFixture.library());
    auto runtimeLibraryPtr = ao::test::requireValue(
      Library::create(runtime, libraryFixture.library(), changes, libraryFixture.root() / "cache"));
    auto& service = runtimeLibraryPtr->taskService();
    auto completedCountPtr = std::make_shared<std::atomic<std::size_t>>(0);
    auto futures = std::vector<async::TaskFuture<Result<std::optional<std::vector<std::byte>>>>>{};
    futures.reserve(kRequestCount);

    for (std::size_t request = 0; request < kRequestCount; ++request)
    {
      futures.push_back(runtime.spawn(countCompletion(completedCountPtr, service.loadResourceAsync(resourceId))));
    }

    REQUIRE(executor.drainUntil([&completedCountPtr] { return completedCountPtr->load() == kRequestCount; }));

    for (auto& future : futures)
    {
      REQUIRE(future.get());
    }

    CHECK(service.resourceCarrierIndexBuildCount() == 1);

    runtime.requestStop();
    runtime.join();
  }

  TEST_CASE("LibraryTaskService - cancelling an interactive resource read suppresses completion",
            "[runtime][regression][library-task][concurrency]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const bytes = std::array{std::byte{0x01}, std::byte{0x02}};
    auto const resourceId = writeResource(libraryFixture.library(), bytes);
    auto executor = QueuedExecutor{};
    auto runtime = async::Runtime{executor};
    auto changes = makeLibraryChanges(executor, libraryFixture.library());
    auto runtimeLibraryPtr = ao::test::requireValue(Library::create(runtime, libraryFixture.library(), changes));
    auto stopSource = std::stop_source{};
    auto completedPtr = std::make_shared<std::atomic_bool>(false);
    auto future = spawnFuture(runtime,
                              runtimeLibraryPtr->taskService().loadResourceAsync(
                                resourceId, ResourceSizeLimit::Interactive, stopSource.get_token()),
                              completedPtr);
    executor.checkQueued();

    REQUIRE(stopSource.request_stop());
    REQUIRE(executor.drainUntil([&completedPtr] { return isReady(completedPtr); }));
    CHECK_THROWS_AS(std::ignore = future.get(), async::OperationCancelled);

    runtime.requestStop();
    runtime.join();
  }

  TEST_CASE("LibraryTaskService - prepareLibraryImportAsync returns failure for invalid path",
            "[runtime][unit][library][task]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto executor = QueuedExecutor{};
    auto runtime = async::Runtime{executor};
    auto changes = makeLibraryChanges(executor, libraryFixture.library());
    auto runtimeLibraryPtr = ao::test::requireValue(Library::create(runtime, libraryFixture.library(), changes));
    auto& service = runtimeLibraryPtr->taskService();

    auto const result = runQueuedTask(
      runtime, executor, service.prepareLibraryImportAsync("/nonexistent_path_123.yaml", ImportMode::Restore));

    REQUIRE_FALSE(result);
    CHECK(result.error().code == Error::Code::IoError);
    CHECK(result.error().message.contains("Failed to read"));
    CHECK(std::string_view{result.error().location.file_name()}.contains("LibraryYamlImporter.cpp"));
  }

  TEST_CASE("LibraryTaskService - import plans bind preview bytes and target state",
            "[runtime][unit][library-import][authorization]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const existingTrackId =
      libraryFixture.addTrack(library::test::TrackSpec{.title = "Existing", .uri = "existing.flac"});
    auto executor = QueuedExecutor{};
    auto runtime = async::Runtime{executor};
    auto changes = makeLibraryChanges(executor, libraryFixture.library());
    auto runtimeLibraryPtr = ao::test::requireValue(Library::create(runtime, libraryFixture.library(), changes));
    auto& service = runtimeLibraryPtr->taskService();
    auto const yamlPath = libraryFixture.root() / "import.yaml";
    writeImportPayload(yamlPath, "Prepared");
    auto planRes = runQueuedTask(runtime, executor, service.prepareLibraryImportAsync(yamlPath, ImportMode::Restore));

    REQUIRE(planRes);
    CHECK(planRes->report().payloadVersion == 5);
    CHECK(planRes->report().payloadMode == ExportMode::Full);
    CHECK(planRes->report().targetScope == ImportTargetScope::Library);
    CHECK(planRes->report().tracksCreated == 1);

    SECTION("unchanged preview applies")
    {
      auto result = runQueuedTask(runtime, executor, service.applyLibraryImportPlanAsync(std::move(*planRes)));

      INFO((result ? "import applied" : result.error().message));
      REQUIRE(result);
      CHECK(result->tracksCreated == 1);
    }

    SECTION("changed source is rejected")
    {
      writeImportPayload(yamlPath, "Changed");
      auto result = runQueuedTask(runtime, executor, service.applyLibraryImportPlanAsync(std::move(*planRes)));

      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::Conflict);
    }

    SECTION("changed target revision is rejected")
    {
      auto deleteRes = runtimeLibraryPtr->writer().deleteTrack(existingTrackId);
      INFO((deleteRes ? "target changed" : deleteRes.error().message));
      REQUIRE(deleteRes);
      auto result = runQueuedTask(runtime, executor, service.applyLibraryImportPlanAsync(std::move(*planRes)));

      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::Conflict);
    }
  }

  TEST_CASE("LibraryTaskService - import plans reject a different runtime over the same library",
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
      auto runtimeLibraryPtr = ao::test::requireValue(Library::create(runtime, libraryFixture.library(), changes));
      auto result = runQueuedTask(
        runtime, executor, runtimeLibraryPtr->taskService().prepareLibraryImportAsync(yamlPath, ImportMode::Restore));

      REQUIRE(result);
      optPlan.emplace(std::move(*result));
    }

    auto otherExecutor = QueuedExecutor{};
    auto otherRuntime = async::Runtime{otherExecutor};
    auto otherChanges = makeLibraryChanges(otherExecutor, libraryFixture.library());
    auto otherLibraryPtr =
      ao::test::requireValue(Library::create(otherRuntime, libraryFixture.library(), otherChanges));
    auto result = runQueuedTask(
      otherRuntime, otherExecutor, otherLibraryPtr->taskService().applyLibraryImportPlanAsync(std::move(*optPlan)));

    REQUIRE_FALSE(result);
    CHECK(result.error().code == Error::Code::Conflict);
  }

  TEST_CASE("LibraryTaskService - cancelled import preparation never enters maintenance",
            "[runtime][unit][library-import][concurrency]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto executor = QueuedExecutor{};
    auto runtime = async::Runtime{executor};
    auto changes = makeLibraryChanges(executor, libraryFixture.library());
    auto runtimeLibraryPtr = ao::test::requireValue(Library::create(runtime, libraryFixture.library(), changes));
    auto const yamlPath = libraryFixture.root() / "import.yaml";
    writeImportPayload(yamlPath, "Prepared");
    auto stopSource = std::stop_source{};

    SECTION("before start")
    {
      REQUIRE(stopSource.request_stop());
      auto future = runtime.spawn(runtimeLibraryPtr->taskService().prepareLibraryImportAsync(
        yamlPath, ImportMode::Restore, stopSource.get_token()));
      CHECK_THROWS_AS(std::ignore = future.get(), async::OperationCancelled);
    }

    SECTION("while callback admission is suspended")
    {
      auto completedPtr = std::make_shared<std::atomic_bool>(false);
      auto future = spawnFuture(runtime,
                                runtimeLibraryPtr->taskService().prepareLibraryImportAsync(
                                  yamlPath, ImportMode::Restore, stopSource.get_token()),
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

  TEST_CASE("LibraryTaskService - import preview cancellation finishes maintenance on the callback owner",
            "[runtime][regression][library-import][concurrency]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto executor = QueuedExecutor{};
    auto runtime = async::Runtime{executor};
    auto changes = makeLibraryChanges(executor, libraryFixture.library());
    auto runtimeLibraryPtr = ao::test::requireValue(Library::create(runtime, libraryFixture.library(), changes));
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
      runtimeLibraryPtr->taskService().prepareLibraryImportAsync(yamlPath, ImportMode::Restore, stopSource.get_token()),
      completedPtr);

    REQUIRE(executor.drainUntil([&] { return isReady(completedPtr); }));
    CHECK_THROWS_AS(std::ignore = future.get(), async::OperationCancelled);
    CHECK(observed == std::vector{LibraryAuthoringState::Maintenance, LibraryAuthoringState::Available});
    CHECK(runtimeLibraryPtr->authoringAvailability().state == LibraryAuthoringState::Available);
    runtime.requestStop();
    runtime.join();
  }

  TEST_CASE("LibraryTaskService - cancellation after import commit preserves mandatory completion",
            "[runtime][regression][library-import][concurrency]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto executor = QueuedExecutor{};
    auto runtime = async::Runtime{executor};
    auto changes = makeLibraryChanges(executor, libraryFixture.library());
    auto runtimeLibraryPtr = ao::test::requireValue(Library::create(runtime, libraryFixture.library(), changes));
    auto& service = runtimeLibraryPtr->taskService();
    auto const yamlPath = libraryFixture.root() / "import.yaml";
    writeImportPayload(yamlPath, "Committed");
    auto prepareCompletedPtr = std::make_shared<std::atomic_bool>(false);
    auto prepareFuture =
      spawnFuture(runtime, service.prepareLibraryImportAsync(yamlPath, ImportMode::Restore), prepareCompletedPtr);

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
      runtime, service.applyLibraryImportPlanAsync(std::move(*planRes), stopSource.get_token()), applyCompletedPtr);

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

  TEST_CASE("LibraryTaskService - exportLibraryAsync returns failure for invalid path",
            "[runtime][unit][library][task]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto executor = QueuedExecutor{};
    auto runtime = async::Runtime{executor};
    auto changes = makeLibraryChanges(executor, libraryFixture.library());
    auto runtimeLibraryPtr = ao::test::requireValue(Library::create(runtime, libraryFixture.library(), changes));
    auto& service = runtimeLibraryPtr->taskService();

    auto const result =
      runQueuedTask(runtime, executor, service.exportLibraryAsync("/root/nonexistent_path_123.yaml", ExportMode::Full));

    REQUIRE_FALSE(result);
    CHECK(result.error().code == Error::Code::IoError);
  }

  TEST_CASE("LibraryTaskService - buildScanPlanAsync succeeds", "[runtime][unit][library-task][scan]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto executor = QueuedExecutor{};
    auto runtime = async::Runtime{executor};
    auto changes = makeLibraryChanges(executor, libraryFixture.library());
    auto runtimeLibraryPtr = ao::test::requireValue(Library::create(runtime, libraryFixture.library(), changes));
    auto& service = runtimeLibraryPtr->taskService();
    std::int32_t progressFinishedCount = 0;
    auto progressFinishedSub =
      service.onProgressFinished([&progressFinishedCount] noexcept { ++progressFinishedCount; });

    auto const result = runQueuedTask(runtime, executor, service.buildScanPlanAsync());

    REQUIRE(result);
    CHECK(progressFinishedCount == 1);
  }

  TEST_CASE("LibraryTaskService - pre-admission cancellation publishes no progress conversation",
            "[runtime][regression][library-task][concurrency]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto executor = QueuedExecutor{};
    auto runtime = async::Runtime{executor};
    auto changes = makeLibraryChanges(executor, libraryFixture.library());
    auto runtimeLibraryPtr = ao::test::requireValue(Library::create(runtime, libraryFixture.library(), changes));
    auto& service = runtimeLibraryPtr->taskService();
    std::int32_t progressCount = 0;
    std::int32_t progressFinishedCount = 0;
    std::int32_t availabilityCount = 0;
    auto progressSubscription =
      service.onProgress([&progressCount](LibraryTaskProgressUpdated const&) noexcept { ++progressCount; });
    auto progressFinishedSubscription =
      service.onProgressFinished([&progressFinishedCount] noexcept { ++progressFinishedCount; });
    auto availabilitySubscription = runtimeLibraryPtr->onAuthoringAvailabilityChanged(
      [&availabilityCount](LibraryAuthoringAvailability const&) noexcept { ++availabilityCount; });
    auto stopSource = std::stop_source{};
    REQUIRE(stopSource.request_stop());
    auto completedPtr = std::make_shared<std::atomic_bool>(false);

    SECTION("scan plan build")
    {
      auto future = spawnFuture(runtime, service.buildScanPlanAsync(stopSource.get_token()), completedPtr);

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
        spawnFuture(runtime, service.applyScanPlanAsync(std::move(*planRes), {}, stopSource.get_token()), completedPtr);

      REQUIRE(executor.drainUntil([&completedPtr] { return isReady(completedPtr); }));
      CHECK(progressCount == 0);
      CHECK(progressFinishedCount == 0);
      CHECK(availabilityCount == 0);
      CHECK(runtimeLibraryPtr->authoringAvailability().state == LibraryAuthoringState::Available);
      requireCancellation(future, runtime);
    }

    SECTION("audio identity backfill")
    {
      auto future = spawnFuture(runtime, service.backfillAudioIdentityAsync(stopSource.get_token()), completedPtr);

      REQUIRE(executor.drainUntil([&completedPtr] { return isReady(completedPtr); }));
      CHECK(progressCount == 0);
      CHECK(progressFinishedCount == 0);
      CHECK(availabilityCount == 0);
      CHECK(runtimeLibraryPtr->authoringAvailability().state == LibraryAuthoringState::Available);
      requireCancellation(future, runtime);
    }
  }

  TEST_CASE("LibraryTaskService - scan progress preserves UTF-8 filenames", "[runtime][regression][library-task]")
  {
    auto const expected = std::string{"\xE8\xAA\xB0\xE3\x81\x8B\xE3\x80\x81\xE6\xB5\xB7\xE3\x82\x92\xE3\x80\x82.flac"};
    auto libraryFixture = MusicLibraryFixture{};
    std::filesystem::copy_file(
      audio::test::requireAudioFixture("basic_metadata.flac"), libraryFixture.root() / utility::pathFromUtf8(expected));
    auto executor = QueuedExecutor{};
    auto runtime = async::Runtime{executor};
    auto changes = makeLibraryChanges(executor, libraryFixture.library());
    auto runtimeLibraryPtr = ao::test::requireValue(Library::create(runtime, libraryFixture.library(), changes));
    auto& service = runtimeLibraryPtr->taskService();
    auto progressEvents = std::vector<LibraryTaskProgressUpdated>{};
    [[maybe_unused]] auto subscription =
      service.onProgress([&](LibraryTaskProgressUpdated const& event) noexcept { progressEvents.push_back(event); });

    auto const result = runQueuedTask(runtime, executor, service.buildScanPlanAsync());

    REQUIRE(result);
    REQUIRE(result->size() == 1);
    CHECK(result->items().front().uri == expected);
    CHECK(
      std::ranges::any_of(progressEvents,
                          [&](LibraryTaskProgressUpdated const& event)
                          { return event.kind == LibraryTaskProgressKind::Scanning && event.subject == expected; }));
  }

  TEST_CASE("LibraryTaskService - applyScanPlanAsync succeeds with empty plan", "[runtime][unit][library-task][scan]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto executor = QueuedExecutor{};
    auto runtime = async::Runtime{executor};
    auto changes = makeLibraryChanges(executor, libraryFixture.library());
    auto observed = std::vector<LibraryChangeSet>{};
    auto changedSubscription =
      changes.onChanged([&observed](LibraryChangeSet const& changeSet) noexcept { observed.push_back(changeSet); });
    auto runtimeLibraryPtr = ao::test::requireValue(Library::create(runtime, libraryFixture.library(), changes));
    auto& service = runtimeLibraryPtr->taskService();

    auto plan = LibraryScan{libraryFixture.library()}.buildPlan().value();
    auto const result = runQueuedTask(runtime, executor, service.applyScanPlanAsync(std::move(plan)));

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

  TEST_CASE("LibraryTaskService - applyScanPlanAsync can defer new audio identity", "[runtime][unit][library][task]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const sourceFile = audio::test::requireAudioFixture("basic_metadata.flac");
    std::filesystem::copy_file(sourceFile, libraryFixture.root() / "song.flac");

    auto executor = QueuedExecutor{};
    auto runtime = async::Runtime{executor};
    auto changes = makeLibraryChanges(executor, libraryFixture.library());
    auto runtimeLibraryPtr = ao::test::requireValue(Library::create(runtime, libraryFixture.library(), changes));
    auto& service = runtimeLibraryPtr->taskService();

    auto scanService = LibraryScan{libraryFixture.library()};
    auto plan = scanService.buildPlan().value();
    REQUIRE(plan.count(ScanClassification::New) == 1);

    auto const result =
      runQueuedTask(runtime,
                    executor,
                    service.applyScanPlanAsync(
                      std::move(plan), ScanApplyOptions{.audioIdentityPolicy = AudioIdentityPolicy::DeferNew}));

    REQUIRE(result);
    REQUIRE(result->insertedIds.size() == 1);
    auto transaction = libraryFixture.library().readTransaction();
    auto optManifest = libraryFixture.library().manifest().reader(transaction).get("song.flac");
    REQUIRE(optManifest);
    CHECK_FALSE(library::hasAudioIdentity(optManifest->audioPayloadLength(), optManifest->audioSignature()));
  }

  TEST_CASE("LibraryTaskService - scan preparation keeps interactive authoring closed",
            "[runtime][unit][library-task][concurrency]")
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
    auto runtimeLibraryPtr = ao::test::requireValue(Library::create(runtime, libraryFixture.library(), changes));
    auto bindingRes = runtimeLibraryPtr->bindTrackTargets(std::array{authoringTarget});
    REQUIRE(bindingRes);
    auto preparationStarted = AsyncTestState<bool>::create(false);
    auto releasePreparation = AsyncBarrier{};
    auto completedPtr = std::make_shared<std::atomic_bool>(false);
    auto future = spawnFuture(runtime,
                              runtimeLibraryPtr->taskService().applyScanPlanAsync(
                                std::move(*planRes),
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
      CHECK(availability.state == LibraryAuthoringState::Maintenance);
      CHECK(availability.maintenanceKind == LibraryMaintenanceKind::ScanApply);

      auto authoringRes =
        runtimeLibraryPtr->writer().updateMetadata(*bindingRes, MetadataPatch{.optTitle = "Must not apply"});
      REQUIRE(authoringRes);
      CHECK(authoringRes->status == TrackAuthoringStatus::Unavailable);

      auto listRes = runtimeLibraryPtr->writer().createList(LibraryWriter::ListDraft{.name = "Blocked"});
      REQUIRE_FALSE(listRes);
      CHECK(listRes.error().code == Error::Code::InvalidState);
    }

    releasePreparation.release();
    REQUIRE(startedInTime);
    REQUIRE(executor.drainUntil([&completedPtr] { return isReady(completedPtr); }));
    REQUIRE(future.get());
    CHECK(runtimeLibraryPtr->authoringAvailability().state == LibraryAuthoringState::Available);
    runtime.requestStop();
    runtime.join();
  }

  TEST_CASE("LibraryTaskService - backfillAudioIdentityAsync fills pending rows", "[runtime][unit][library][task]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const sourceFile = audio::test::requireAudioFixture("basic_metadata.flac");
    std::filesystem::copy_file(sourceFile, libraryFixture.root() / "song.flac");

    auto executor = QueuedExecutor{};
    auto runtime = async::Runtime{executor};
    auto changes = makeLibraryChanges(executor, libraryFixture.library());
    auto runtimeLibraryPtr = ao::test::requireValue(Library::create(runtime, libraryFixture.library(), changes));
    auto& service = runtimeLibraryPtr->taskService();

    auto scanService = LibraryScan{libraryFixture.library()};
    auto plan = scanService.buildPlan().value();
    auto const applyRes =
      runQueuedTask(runtime,
                    executor,
                    service.applyScanPlanAsync(
                      std::move(plan), ScanApplyOptions{.audioIdentityPolicy = AudioIdentityPolicy::DeferNew}));
    REQUIRE(applyRes);

    auto const backfillRes = runQueuedTask(runtime, executor, service.backfillAudioIdentityAsync());

    REQUIRE(backfillRes);
    CHECK(backfillRes->completedCount == 1);
    CHECK(backfillRes->skippedCount == 0);
    CHECK(backfillRes->failureCount == 0);

    auto transaction = libraryFixture.library().readTransaction();
    auto optManifest = libraryFixture.library().manifest().reader(transaction).get("song.flac");
    REQUIRE(optManifest);
    CHECK(library::hasAudioIdentity(optManifest->audioPayloadLength(), optManifest->audioSignature()));
  }

  TEST_CASE("LibraryTaskService - applyScanPlanAsync reports progress while applying plan",
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
    auto runtimeLibraryPtr = ao::test::requireValue(Library::create(runtime, libraryFixture.library(), changes));
    auto& service = runtimeLibraryPtr->taskService();

    auto progressEvents = std::vector<LibraryTaskProgressUpdated>{};
    auto sub =
      runtimeLibraryPtr->taskService().onProgress([&](auto const& ev) noexcept { progressEvents.push_back(ev); });
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
      service.applyScanPlanAsync(
        std::move(plan), {}, {}, {}, [&failureCallbackCount](ScanFailure const&) { ++failureCallbackCount; }));

    REQUIRE(result);
    CHECK(result->insertedIds.empty());
    CHECK(result->mutatedIds.empty());
    CHECK(result->relinkedIds.empty());
    CHECK(result->failureCount == 2);
    CHECK(failureCallbackCount == 2);

    REQUIRE(progressEvents.size() == 2);
    CHECK(progressEvents[0].kind == LibraryTaskProgressKind::Updating);
    CHECK(progressEvents[0].subject == expectedNames[0]);
    CHECK(progressEvents[0].fraction == 0.0);
    CHECK(progressEvents[1].kind == LibraryTaskProgressKind::Updating);
    CHECK(progressEvents[1].subject == expectedNames[1]);
    CHECK(progressEvents[1].fraction == 0.5);

    for (auto const& event : progressEvents)
    {
      CHECK(event.fraction >= 0.0);
      CHECK(event.fraction <= 1.0);
      CHECK_FALSE(event.subject.empty());
    }
  }

  TEST_CASE("LibraryTaskService - applyScanPlanAsync forwards cancellation to scan executor",
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
    auto runtimeLibraryPtr = ao::test::requireValue(Library::create(runtime, libraryFixture.library(), changes));
    auto& service = runtimeLibraryPtr->taskService();

    auto scanService = LibraryScan{libraryFixture.library()};
    auto plan = scanService.buildPlan().value();
    REQUIRE(plan.count(ScanClassification::New) == 1);

    auto fingerprintingEntered = AsyncTestState<bool>::create(false);
    auto fingerprintingRelease = AsyncBarrier{};
    auto sawCancellation = AsyncTestState<bool>::create(false);
    auto progressFinished = AsyncTestState<bool>::create(false);
    auto progressFinishedSub =
      runtimeLibraryPtr->taskService().onProgressFinished([progressFinished] noexcept { progressFinished.set(true); });

    auto taskHandle = runtime.spawnCancellable(
      [service = &service,
       plan = std::move(plan),
       fingerprintingEntered,
       fingerprintingBarrier = &fingerprintingRelease,
       sawCancellation](std::stop_token const stopToken) mutable
      {
        return applyScanPlanAndRecordCancellation(
          service, std::move(plan), fingerprintingEntered, fingerprintingBarrier, sawCancellation, stopToken);
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

    runtime.requestStop();
    runtime.join();
  }

  TEST_CASE("LibraryTaskService - throwing scan progress callback propagates after maintenance cleanup",
            "[runtime][regression][library-task][concurrency]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto executor = QueuedExecutor{};
    auto runtime = async::Runtime{executor};
    auto changes = makeLibraryChanges(executor, libraryFixture.library());
    auto runtimeLibraryPtr = ao::test::requireValue(Library::create(runtime, libraryFixture.library(), changes));
    auto& service = runtimeLibraryPtr->taskService();
    auto const sourceFile = audio::test::requireAudioFixture("basic_metadata.flac");
    auto const targetFile = libraryFixture.root() / "song.flac";
    std::filesystem::copy_file(sourceFile, targetFile);
    auto plan = LibraryScan{libraryFixture.library()}.buildPlan().value();
    std::int32_t callbackCount = 0;
    auto completedPtr = std::make_shared<std::atomic_bool>(false);
    auto future =
      spawnFuture(runtime,
                  service.applyScanPlanAsync(std::move(plan),
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
    runtime.requestStop();
    runtime.join();
  }

  TEST_CASE("LibraryTaskService - throwing backfill progress callback propagates after maintenance cleanup",
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
    auto runtimeLibraryPtr = ao::test::requireValue(Library::create(runtime, libraryFixture.library(), changes));
    auto& service = runtimeLibraryPtr->taskService();
    std::int32_t callbackCount = 0;
    auto completedPtr = std::make_shared<std::atomic_bool>(false);
    auto future = spawnFuture(
      runtime,
      service.backfillAudioIdentityAsync({},
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
    runtime.requestStop();
    runtime.join();
  }

  TEST_CASE("LibraryTaskService - apply cancellation finishes maintenance before propagation",
            "[runtime][regression][library-task][concurrency]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto executor = QueuedExecutor{};
    auto runtime = async::Runtime{executor};
    auto changes = makeLibraryChanges(executor, libraryFixture.library());
    auto runtimeLibraryPtr = ao::test::requireValue(Library::create(runtime, libraryFixture.library(), changes));
    auto& service = runtimeLibraryPtr->taskService();
    auto const sourceFile = audio::test::requireAudioFixture("basic_metadata.flac");
    auto const targetFile = libraryFixture.root() / "song.flac";
    std::filesystem::copy_file(sourceFile, targetFile);
    auto plan = LibraryScan{libraryFixture.library()}.buildPlan().value();
    auto stopSource = std::stop_source{};
    auto progressFinished = AsyncTestState<bool>::create(false);
    auto progressFinishedSub = service.onProgressFinished([progressFinished] noexcept { progressFinished.set(true); });
    auto completedPtr = std::make_shared<std::atomic_bool>(false);
    auto future = spawnFuture(
      runtime,
      service.applyScanPlanAsync(std::move(plan),
                                 {},
                                 stopSource.get_token(),
                                 [&stopSource](ScanApplyProgress const&) { std::ignore = stopSource.request_stop(); }),
      completedPtr);

    REQUIRE(executor.drainUntil([&] { return isReady(completedPtr); }));
    CHECK(runtimeLibraryPtr->authoringAvailability().state == LibraryAuthoringState::Available);
    CHECK(progressFinished.load());
    requireCancellation(future, runtime);
  }

  TEST_CASE("LibraryTaskService - backfill cancellation finishes maintenance before propagation",
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
    auto runtimeLibraryPtr = ao::test::requireValue(Library::create(runtime, libraryFixture.library(), changes));
    auto& service = runtimeLibraryPtr->taskService();
    auto stopSource = std::stop_source{};
    auto progressFinished = AsyncTestState<bool>::create(false);
    auto progressFinishedSub = service.onProgressFinished([progressFinished] noexcept { progressFinished.set(true); });
    auto completedPtr = std::make_shared<std::atomic_bool>(false);
    auto future = spawnFuture(runtime,
                              service.backfillAudioIdentityAsync(stopSource.get_token(),
                                                                 [&stopSource](AudioIdentityIndexProgress const&)
                                                                 { std::ignore = stopSource.request_stop(); }),
                              completedPtr);

    REQUIRE(executor.drainUntil([&] { return isReady(completedPtr); }));
    CHECK(runtimeLibraryPtr->authoringAvailability().state == LibraryAuthoringState::Available);
    CHECK(progressFinished.load());
    requireCancellation(future, runtime);
  }
} // namespace ao::rt::test
