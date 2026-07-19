// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "runtime/library/ScanApplyOperation.h"
#include "test/unit/RuntimeTestSupport.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include <ao/Error.h>
#include <ao/Exception.h>
#include <ao/async/Executor.h>
#include <ao/async/OperationCancelled.h>
#include <ao/async/Runtime.h>
#include <ao/async/Task.h>
#include <ao/library/AudioIdentity.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/TrackStore.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryScan.h>
#include <ao/rt/library/LibraryTaskService.h>
#include <ao/rt/library/LibraryYamlExporter.h>
#include <ao/rt/library/LibraryYamlImporter.h>
#include <ao/rt/library/ScanPlan.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    class InjectedLibraryTaskFailure final : public Exception
    {
    public:
      using Exception::Exception;
    };

    class FaultOrderingExecutor final : public async::Executor
    {
    public:
      bool isCurrent() const noexcept override { return std::this_thread::get_id() == _ownerThreadId; }

      void dispatch(std::move_only_function<void()> task) override
      {
        // Resume admission, maintenance-state notification, and the injected
        // progress fault run inline. Cleanup and maintenance exit stay queued.
        if (_dispatchCount.fetch_add(1) < 3)
        {
          task();
          return;
        }

        auto const lock = std::scoped_lock{_mutex};
        _tasks.push_back(std::move(task));
      }

      void defer(std::move_only_function<void()> task) override { dispatch(std::move(task)); }

      std::size_t dispatchCount() const noexcept { return _dispatchCount.load(); }

      std::size_t queuedCount() const
      {
        auto const lock = std::scoped_lock{_mutex};
        return _tasks.size();
      }

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

    private:
      std::thread::id _ownerThreadId = std::this_thread::get_id();
      std::atomic_size_t _dispatchCount{0};
      mutable std::mutex _mutex;
      std::deque<std::move_only_function<void()>> _tasks;
    };

    template<typename Future>
    void requireInjectedFailure(Future& future)
    {
      auto const exceptionPtr = captureTaskFutureException(future);
      REQUIRE(exceptionPtr);
      bool sawInjectedFailure = false;

      try
      {
        std::rethrow_exception(exceptionPtr);
      }
      catch (InjectedLibraryTaskFailure const& error)
      {
        sawInjectedFailure = true;
        CHECK(std::string_view{error.what()} == "injected library task failure");
      }

      REQUIRE(sawInjectedFailure);
    }

    template<typename Future>
    void requireFaultCleanupOrdering(Future& future,
                                     std::stop_source& stopSource,
                                     FaultOrderingExecutor& executor,
                                     std::vector<LibraryChanges::LibraryTaskCompletionStatus>& completionStatuses)
    {
      requireInjectedFailure(future);

      CHECK(completionStatuses.empty());
      CHECK(executor.dispatchCount() == 5);
      REQUIRE(executor.queuedCount() == 2);

      CHECK(stopSource.request_stop());
      REQUIRE(executor.runOne());

      CHECK(completionStatuses == std::vector{LibraryChanges::LibraryTaskCompletionStatus::Failed});
      REQUIRE(executor.runOne());
      CHECK(executor.queuedCount() == 0);
    }

    async::Task<void> applyScanPlanAndRecordCancellation(LibraryTaskService* service,
                                                         ScanPlan plan,
                                                         AsyncTestState<bool> sawCancellation,
                                                         std::stop_token const stopToken)
    {
      try
      {
        [[maybe_unused]] auto result = co_await service->applyScanPlanAsync(std::move(plan), {}, stopToken);
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
      yaml << "version: 2\n"
              "export_mode: full\n"
              "library:\n"
              "  tracks:\n"
              "    - uri: imported.flac\n"
              "      title: \""
           << title
           << "\"\n"
              "  lists: []\n";
    }

    class CompletionFlag final
    {
    public:
      explicit CompletionFlag(std::shared_ptr<std::atomic_bool> completedPtr)
        : _completedPtr{std::move(completedPtr)}
      {
      }

      ~CompletionFlag() { _completedPtr->store(true); }

      CompletionFlag(CompletionFlag const&) = delete;
      CompletionFlag& operator=(CompletionFlag const&) = delete;
      CompletionFlag(CompletionFlag&&) = delete;
      CompletionFlag& operator=(CompletionFlag&&) = delete;

    private:
      std::shared_ptr<std::atomic_bool> _completedPtr;
    };

    template<typename T>
    async::Task<T> flagCompletion(std::shared_ptr<std::atomic_bool> completedPtr, async::Task<T> task)
    {
      [[maybe_unused]] auto completionFlag = CompletionFlag{std::move(completedPtr)};
      co_return co_await std::move(task);
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
  } // namespace

  TEST_CASE("LibraryTaskService - prepareLibraryImportAsync returns failure for invalid path",
            "[runtime][unit][library][task]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto executor = InlineExecutor{};
    auto runtime = async::Runtime{executor};
    auto changes = LibraryChanges{};
    auto runtimeLibrary = Library{runtime, libraryFixture.library(), changes};
    auto& service = runtimeLibrary.taskService();

    auto future = runtime.spawn(service.prepareLibraryImportAsync("/nonexistent_path_123.yaml", ImportMode::Restore));
    auto const result = future.get();

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
    auto executor = InlineExecutor{};
    auto runtime = async::Runtime{executor};
    auto changes = LibraryChanges{};
    auto runtimeLibrary = Library{runtime, libraryFixture.library(), changes};
    auto& service = runtimeLibrary.taskService();
    auto const yamlPath = libraryFixture.root() / "import.yaml";
    writeImportPayload(yamlPath, "Prepared");
    auto planResult = runtime.spawn(service.prepareLibraryImportAsync(yamlPath, ImportMode::Restore)).get();

    REQUIRE(planResult);
    CHECK(planResult->report().payloadVersion == 2);
    CHECK(planResult->report().payloadMode == ExportMode::Full);
    CHECK(planResult->report().targetScope == ImportTargetScope::Library);
    CHECK(planResult->report().tracksCreated == 1);

    SECTION("unchanged preview applies")
    {
      auto result = runtime.spawn(service.applyLibraryImportPlanAsync(std::move(*planResult))).get();

      INFO((result ? "import applied" : result.error().message));
      REQUIRE(result);
      CHECK(result->tracksCreated == 1);
    }

    SECTION("changed source is rejected")
    {
      writeImportPayload(yamlPath, "Changed");
      auto result = runtime.spawn(service.applyLibraryImportPlanAsync(std::move(*planResult))).get();

      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::Conflict);
    }

    SECTION("changed target revision is rejected")
    {
      auto deleteResult = runtimeLibrary.writer().deleteTrack(existingTrackId);
      INFO((deleteResult ? "target changed" : deleteResult.error().message));
      REQUIRE(deleteResult);
      auto result = runtime.spawn(service.applyLibraryImportPlanAsync(std::move(*planResult))).get();

      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::Conflict);
    }

    SECTION("consumed plan cannot be applied again")
    {
      auto plan = std::move(*planResult);
      auto const firstApplication = runtime.spawn(service.applyLibraryImportPlanAsync(std::move(plan))).get();
      REQUIRE(firstApplication);

      // LibraryImportPlan specifies an empty moved-from state so callers receive
      // InvalidState when an already-consumed authorization is submitted again.
      // NOLINTNEXTLINE(bugprone-use-after-move)
      auto const reused = runtime.spawn(service.applyLibraryImportPlanAsync(std::move(plan))).get();
      REQUIRE_FALSE(reused);
      CHECK(reused.error().code == Error::Code::InvalidState);
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
      auto executor = InlineExecutor{};
      auto runtime = async::Runtime{executor};
      auto changes = LibraryChanges{};
      auto runtimeLibrary = Library{runtime, libraryFixture.library(), changes};
      auto result =
        runtime.spawn(runtimeLibrary.taskService().prepareLibraryImportAsync(yamlPath, ImportMode::Restore)).get();

      REQUIRE(result);
      optPlan.emplace(std::move(*result));
    }

    auto otherExecutor = InlineExecutor{};
    auto otherRuntime = async::Runtime{otherExecutor};
    auto otherChanges = LibraryChanges{};
    auto otherLibrary = Library{otherRuntime, libraryFixture.library(), otherChanges};
    auto result = otherRuntime.spawn(otherLibrary.taskService().applyLibraryImportPlanAsync(std::move(*optPlan))).get();

    REQUIRE_FALSE(result);
    CHECK(result.error().code == Error::Code::Conflict);
  }

  TEST_CASE("LibraryTaskService - cancelled import preparation never enters maintenance",
            "[runtime][unit][library-import][concurrency]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto executor = QueuedExecutor{};
    auto runtime = async::Runtime{executor};
    auto changes = LibraryChanges{};
    auto runtimeLibrary = Library{runtime, libraryFixture.library(), changes};
    auto const yamlPath = libraryFixture.root() / "import.yaml";
    writeImportPayload(yamlPath, "Prepared");
    auto stopSource = std::stop_source{};

    SECTION("before start")
    {
      REQUIRE(stopSource.request_stop());
      auto future = runtime.spawn(
        runtimeLibrary.taskService().prepareLibraryImportAsync(yamlPath, ImportMode::Restore, stopSource.get_token()));
      CHECK_THROWS_AS(std::ignore = future.get(), async::OperationCancelled);
    }

    SECTION("while callback admission is suspended")
    {
      auto completedPtr = std::make_shared<std::atomic_bool>(false);
      auto future = spawnFuture(
        runtime,
        runtimeLibrary.taskService().prepareLibraryImportAsync(yamlPath, ImportMode::Restore, stopSource.get_token()),
        completedPtr);
      executor.checkQueued();

      REQUIRE(stopSource.request_stop());
      REQUIRE(executor.drainUntil([&completedPtr] { return isReady(completedPtr); }));
      CHECK_THROWS_AS(std::ignore = future.get(), async::OperationCancelled);
    }

    CHECK(runtimeLibrary.authoringAvailability().state == LibraryAuthoringState::Available);
    runtime.requestStop();
    runtime.join();
  }

  TEST_CASE("LibraryTaskService - cancellation after import commit preserves mandatory completion",
            "[runtime][regression][library-import][concurrency]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto executor = QueuedExecutor{};
    auto runtime = async::Runtime{executor};
    auto changes = LibraryChanges{};
    auto runtimeLibrary = Library{runtime, libraryFixture.library(), changes};
    auto& service = runtimeLibrary.taskService();
    auto const yamlPath = libraryFixture.root() / "import.yaml";
    writeImportPayload(yamlPath, "Committed");
    auto prepareCompletedPtr = std::make_shared<std::atomic_bool>(false);
    auto prepareFuture =
      spawnFuture(runtime, service.prepareLibraryImportAsync(yamlPath, ImportMode::Restore), prepareCompletedPtr);

    REQUIRE(executor.drainUntil([&prepareCompletedPtr] { return isReady(prepareCompletedPtr); }));
    auto planResult = prepareFuture.get();
    REQUIRE(planResult);
    executor.drain();
    REQUIRE(runtimeLibrary.authoringAvailability().state == LibraryAuthoringState::Available);

    auto committed = AsyncTestState<bool>::create(false);
    auto changeSubscription = changes.onChanged([committed](LibraryChangeSet const&) { committed.set(true); });
    auto stopSource = std::stop_source{};
    auto applyCompletedPtr = std::make_shared<std::atomic_bool>(false);
    auto applyFuture = spawnFuture(
      runtime, service.applyLibraryImportPlanAsync(std::move(*planResult), stopSource.get_token()), applyCompletedPtr);

    REQUIRE(executor.drainUntil([&committed] { return committed.load(); }));
    REQUIRE(stopSource.request_stop());
    REQUIRE(executor.drainUntil([&applyCompletedPtr] { return isReady(applyCompletedPtr); }));
    auto result = applyFuture.get();

    REQUIRE(result);
    CHECK(result->tracksCreated == 1);
    executor.drain();
    CHECK(runtimeLibrary.authoringAvailability().state == LibraryAuthoringState::Available);
    runtime.requestStop();
    runtime.join();
  }

  TEST_CASE("LibraryTaskService - exportLibraryAsync returns failure for invalid path",
            "[runtime][unit][library][task]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto executor = InlineExecutor{};
    auto runtime = async::Runtime{executor};
    auto changes = LibraryChanges{};
    auto runtimeLibrary = Library{runtime, libraryFixture.library(), changes};
    auto& service = runtimeLibrary.taskService();

    auto future = runtime.spawn(service.exportLibraryAsync("/root/nonexistent_path_123.yaml", ExportMode::Full));
    auto const result = future.get();

    REQUIRE_FALSE(result);
    CHECK(result.error().code == Error::Code::IoError);
  }

  TEST_CASE("LibraryTaskService - buildScanPlanAsync succeeds", "[runtime][unit][library-task][scan]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto executor = InlineExecutor{};
    auto runtime = async::Runtime{executor};
    auto changes = LibraryChanges{};
    auto runtimeLibrary = Library{runtime, libraryFixture.library(), changes};
    auto& service = runtimeLibrary.taskService();

    auto future = runtime.spawn(service.buildScanPlanAsync());
    auto const result = future.get();

    REQUIRE(result);
  }

  TEST_CASE("LibraryTaskService - applyScanPlanAsync succeeds with empty plan", "[runtime][unit][library-task][scan]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto executor = InlineExecutor{};
    auto runtime = async::Runtime{executor};
    auto changes = LibraryChanges{};
    auto runtimeLibrary = Library{runtime, libraryFixture.library(), changes};
    auto& service = runtimeLibrary.taskService();

    auto plan = LibraryScan{libraryFixture.library()}.buildPlan().value();
    auto future = runtime.spawn(service.applyScanPlanAsync(std::move(plan)));
    auto const result = future.get();

    REQUIRE(result);
    CHECK(result->insertedIds.empty());
    CHECK(result->mutatedIds.empty());
    CHECK(result->relinkedIds.empty());
    CHECK(result->failureCount == 0);
  }

  TEST_CASE("LibraryTaskService - applyScanPlanAsync can defer new audio identity", "[runtime][unit][library][task]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const sourceFile = audio::test::requireAudioFixture("basic_metadata.flac");
    std::filesystem::copy_file(sourceFile, libraryFixture.root() / "song.flac");

    auto executor = InlineExecutor{};
    auto runtime = async::Runtime{executor};
    auto changes = LibraryChanges{};
    auto runtimeLibrary = Library{runtime, libraryFixture.library(), changes};
    auto& service = runtimeLibrary.taskService();

    auto scanService = LibraryScan{libraryFixture.library()};
    auto plan = scanService.buildPlan().value();
    REQUIRE(plan.count(ScanClassification::New) == 1);

    auto future = runtime.spawn(service.applyScanPlanAsync(
      std::move(plan), ScanApplyOptions{.audioIdentityPolicy = AudioIdentityPolicy::DeferNew}));
    auto const result = future.get();

    REQUIRE(result);
    REQUIRE(result->insertedIds.size() == 1);
    auto transaction = libraryFixture.library().readTransaction();
    auto manifestResult = libraryFixture.library().manifest().reader(transaction).get("song.flac");
    REQUIRE(manifestResult);
    CHECK_FALSE(library::hasAudioIdentity(manifestResult->audioPayloadLength(), manifestResult->audioSignature()));
  }

  TEST_CASE("LibraryTaskService - scan preparation keeps interactive authoring closed",
            "[runtime][unit][library-task][concurrency]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const authoringTarget = libraryFixture.addTrack("Before");
    auto const sourceFile = audio::test::requireAudioFixture("basic_metadata.flac");
    std::filesystem::copy_file(sourceFile, libraryFixture.root() / "song.flac");
    auto scanService = LibraryScan{libraryFixture.library()};
    auto planResult = scanService.buildPlan();
    REQUIRE(planResult);

    auto executor = InlineExecutor{};
    auto runtime = async::Runtime{executor};
    auto changes = LibraryChanges{};
    auto runtimeLibrary = Library{runtime, libraryFixture.library(), changes};
    auto bindingResult = runtimeLibrary.bindTrackTargets(std::array{authoringTarget});
    REQUIRE(bindingResult);
    auto preparationStarted = AsyncTestState<bool>::create(false);
    auto releasePreparation = AsyncBarrier{};
    auto future = runtime.spawn(runtimeLibrary.taskService().applyScanPlanAsync(
      std::move(*planResult),
      {},
      {},
      [&preparationStarted, &releasePreparation](ScanApplyProgress const&)
      {
        if (!preparationStarted.load())
        {
          preparationStarted.set(true);
          releasePreparation.wait();
        }
      }));

    auto const startedInTime = preparationStarted.waitUntil(true);

    if (startedInTime)
    {
      auto const availability = runtimeLibrary.authoringAvailability();
      CHECK(availability.state == LibraryAuthoringState::Maintenance);
      CHECK(availability.maintenanceKind == LibraryMaintenanceKind::ScanApply);

      auto authoringResult =
        runtimeLibrary.writer().updateMetadata(*bindingResult, MetadataPatch{.optTitle = "Must not apply"});
      REQUIRE(authoringResult);
      CHECK(authoringResult->status == TrackAuthoringStatus::Unavailable);

      auto listResult = runtimeLibrary.writer().createList(
        LibraryWriter::ListDraft{.kind = LibraryWriter::ListKind::Manual, .name = "Blocked"});
      REQUIRE_FALSE(listResult);
      CHECK(listResult.error().code == Error::Code::InvalidState);
    }

    releasePreparation.release();
    REQUIRE(startedInTime);
    REQUIRE(future.get());
    CHECK(runtimeLibrary.authoringAvailability().state == LibraryAuthoringState::Available);
    runtime.requestStop();
    runtime.join();
  }

  TEST_CASE("LibraryTaskService - backfillAudioIdentityAsync fills pending rows", "[runtime][unit][library][task]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const sourceFile = audio::test::requireAudioFixture("basic_metadata.flac");
    std::filesystem::copy_file(sourceFile, libraryFixture.root() / "song.flac");

    auto executor = InlineExecutor{};
    auto runtime = async::Runtime{executor};
    auto changes = LibraryChanges{};
    auto runtimeLibrary = Library{runtime, libraryFixture.library(), changes};
    auto& service = runtimeLibrary.taskService();

    auto scanService = LibraryScan{libraryFixture.library()};
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

    auto transaction = libraryFixture.library().readTransaction();
    auto manifestResult = libraryFixture.library().manifest().reader(transaction).get("song.flac");
    REQUIRE(manifestResult);
    CHECK(library::hasAudioIdentity(manifestResult->audioPayloadLength(), manifestResult->audioSignature()));
  }

  TEST_CASE("LibraryTaskService - applyScanPlanAsync reports progress while applying plan",
            "[runtime][unit][library-task][scan]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const sourceFile = audio::test::requireAudioFixture("basic_metadata.flac");
    auto const firstFile = libraryFixture.root() / "first.flac";
    auto const secondFile = libraryFixture.root() / "second.flac";
    std::filesystem::copy_file(sourceFile, firstFile);
    std::filesystem::copy_file(sourceFile, secondFile);
    auto executor = InlineExecutor{};
    auto runtime = async::Runtime{executor};
    auto changes = LibraryChanges{};
    auto runtimeLibrary = Library{runtime, libraryFixture.library(), changes};
    auto& service = runtimeLibrary.taskService();

    auto progressEvents = std::vector<LibraryChanges::LibraryTaskProgressUpdated>{};
    auto sub = changes.onLibraryTaskProgress([&](auto const& ev) { progressEvents.push_back(ev); });
    auto plan = LibraryScan{libraryFixture.library()}.buildPlan().value();
    auto expectedNames = std::vector<std::string>{};

    for (auto const& item : plan.items())
    {
      expectedNames.push_back(item.fullPath.filename().string());
    }

    std::filesystem::remove(firstFile);
    std::filesystem::remove(secondFile);

    auto future = runtime.spawn(service.applyScanPlanAsync(std::move(plan)));
    auto const result = future.get();

    REQUIRE(result);
    CHECK(result->insertedIds.empty());
    CHECK(result->mutatedIds.empty());
    CHECK(result->relinkedIds.empty());
    CHECK(result->failureCount == 2);

    REQUIRE(progressEvents.size() == 2);
    CHECK(progressEvents[0].kind == LibraryChanges::LibraryTaskProgressKind::Updating);
    CHECK(progressEvents[0].subject == expectedNames[0]);
    CHECK(progressEvents[0].fraction == 0.0);
    CHECK(progressEvents[1].kind == LibraryChanges::LibraryTaskProgressKind::Updating);
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

    auto executor = InlineExecutor{};
    auto runtime = async::Runtime{executor};
    auto changes = LibraryChanges{};
    auto runtimeLibrary = Library{runtime, libraryFixture.library(), changes};
    auto& service = runtimeLibrary.taskService();

    auto scanService = LibraryScan{libraryFixture.library()};
    auto plan = scanService.buildPlan().value();
    REQUIRE(plan.count(ScanClassification::New) == 1);

    auto sawFingerprinting = AsyncTestState<bool>::create(false);
    auto sawCancellation = AsyncTestState<bool>::create(false);
    auto completionStatus = AsyncTestState<LibraryChanges::LibraryTaskCompletionStatus>::create(
      LibraryChanges::LibraryTaskCompletionStatus::Succeeded);
    auto sub = changes.onLibraryTaskProgress(
      [&](LibraryChanges::LibraryTaskProgressUpdated const& event)
      {
        if (event.kind == LibraryChanges::LibraryTaskProgressKind::Fingerprinting && event.subject == "song.flac" &&
            !sawFingerprinting.load())
        {
          sawFingerprinting.set(true);
        }
      });
    auto completionSub = changes.onLibraryTaskCompleted(
      [completionStatus](LibraryChanges::LibraryTaskCompleted const& event) { completionStatus.set(event.status); });

    auto taskHandle = runtime.spawnCancellable(
      [service = &service, plan = std::move(plan), sawCancellation](std::stop_token const stopToken) mutable
      { return applyScanPlanAndRecordCancellation(service, std::move(plan), sawCancellation, stopToken); });

    REQUIRE(sawFingerprinting.waitUntil(true));
    taskHandle.reset();
    REQUIRE(sawCancellation.waitUntil(true));
    CHECK(sawFingerprinting.load());
    CHECK(completionStatus.load() == LibraryChanges::LibraryTaskCompletionStatus::Cancelled);

    auto transaction = libraryFixture.library().readTransaction();
    auto trackReader = libraryFixture.library().tracks().reader(transaction);
    auto manifestReader = libraryFixture.library().manifest().reader(transaction);
    CHECK(trackReader.begin() == trackReader.end());
    CHECK(manifestReader.begin() == manifestReader.end());
    CHECK(runtimeLibrary.authoringAvailability().state == LibraryAuthoringState::Available);

    runtime.requestStop();
    runtime.join();
  }

  TEST_CASE("LibraryTaskService - apply fault queues cleanup before preserving the exception",
            "[runtime][regression][library-task][concurrency]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto executor = FaultOrderingExecutor{};
    auto runtime = async::Runtime{executor};
    auto changes = LibraryChanges{};
    auto runtimeLibrary = Library{runtime, libraryFixture.library(), changes};
    auto& service = runtimeLibrary.taskService();
    auto completionStatuses = std::vector<LibraryChanges::LibraryTaskCompletionStatus>{};
    auto completionSubscription = changes.onLibraryTaskCompleted([&](LibraryChanges::LibraryTaskCompleted const& event)
                                                                 { completionStatuses.push_back(event.status); });
    auto progressSubscription =
      changes.onLibraryTaskProgress([](LibraryChanges::LibraryTaskProgressUpdated const&)
                                    { throwException<InjectedLibraryTaskFailure>("injected library task failure"); });
    auto stopSource = std::stop_source{};
    auto const sourceFile = audio::test::requireAudioFixture("basic_metadata.flac");
    auto const targetFile = libraryFixture.root() / "missing.flac";
    std::filesystem::copy_file(sourceFile, targetFile);
    auto plan = LibraryScan{libraryFixture.library()}.buildPlan().value();
    std::filesystem::remove(targetFile);

    auto future = runtime.spawn(service.applyScanPlanAsync(std::move(plan), {}, stopSource.get_token()));

    requireFaultCleanupOrdering(future, stopSource, executor, completionStatuses);
    runtime.requestStop();
    runtime.join();
  }

  TEST_CASE("LibraryTaskService - backfill fault queues cleanup before preserving the exception",
            "[runtime][regression][library-task][concurrency]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const sourceFile = audio::test::requireAudioFixture("basic_metadata.flac");
    std::filesystem::copy_file(sourceFile, libraryFixture.root() / "song.flac");
    auto scanService = LibraryScan{libraryFixture.library()};
    auto planResult = scanService.buildPlan();
    REQUIRE(planResult);
    auto applyResult = ScanApplyOperation{libraryFixture.library(),
                                          std::move(*planResult),
                                          {},
                                          {},
                                          ScanApplyOptions{.audioIdentityPolicy = AudioIdentityPolicy::DeferNew}}
                         .run();
    REQUIRE(applyResult);
    REQUIRE(applyResult->insertedIds.size() == 1);

    auto executor = FaultOrderingExecutor{};
    auto runtime = async::Runtime{executor};
    auto changes = LibraryChanges{};
    auto runtimeLibrary = Library{runtime, libraryFixture.library(), changes};
    auto& service = runtimeLibrary.taskService();
    auto completionStatuses = std::vector<LibraryChanges::LibraryTaskCompletionStatus>{};
    auto completionSubscription = changes.onLibraryTaskCompleted([&](LibraryChanges::LibraryTaskCompleted const& event)
                                                                 { completionStatuses.push_back(event.status); });
    auto progressSubscription =
      changes.onLibraryTaskProgress([](LibraryChanges::LibraryTaskProgressUpdated const&)
                                    { throwException<InjectedLibraryTaskFailure>("injected library task failure"); });
    auto stopSource = std::stop_source{};

    auto future = runtime.spawn(service.backfillAudioIdentityAsync(stopSource.get_token()));

    requireFaultCleanupOrdering(future, stopSource, executor, completionStatuses);
    runtime.requestStop();
    runtime.join();
  }
} // namespace ao::rt::test
