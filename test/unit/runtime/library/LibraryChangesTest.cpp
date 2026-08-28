// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/rt/library/LibraryChanges.h>

#include "runtime/library/LibraryWriteLane.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/library/WritableLibraryTestSupport.h"
#include "test/unit/runtime/AsyncTestSupport.h"
#include "test/unit/runtime/ExecutorTestSupport.h"
#include "test/unit/runtime/RuntimeLibraryTestSupport.h"
#include "test/unit/runtime/library/LibraryWriteLaneTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/OperationCancelled.h>
#include <ao/async/Subscription.h>
#include <ao/async/Task.h>
#include <ao/compat/MoveOnlyFunction.h>
#include <ao/library/LibraryWrite.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackStore.h>
#include <ao/library/WriteTransaction.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/rt/library/LibraryCommands.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    class CompletionBeforeForeignDispatchReturnsExecutor final : public async::Executor
    {
    public:
      bool isCurrent() const noexcept override { return _delegate.isCurrent(); }

      void dispatch(compat::MoveOnlyFunction<void()> task) override
      {
        if (isCurrent())
        {
          task();
          return;
        }

        auto completed = AsyncTestState<bool>::create(false);
        // Keep the submitting thread inside dispatch until the owner has run
        // the publication, forcing completion to win the rendezvous.
        _delegate.dispatch(
          [task = std::move(task), completed] mutable
          {
            task();
            completed.set(true);
          });

        if (!completed.waitUntil(true))
        {
          throw std::runtime_error{"Timed out waiting for controlled foreign dispatch completion"};
        }

        _foreignDispatchReturned.set(true);
      }

      void defer(compat::MoveOnlyFunction<void()> task) override { _delegate.defer(std::move(task)); }
      void drain() { _delegate.drain(); }
      bool waitUntilQueued() const { return _delegate.waitUntilQueued(); }
      bool foreignDispatchReturned() const { return _foreignDispatchReturned.load(); }

    private:
      QueuedExecutor _delegate;
      AsyncTestState<bool> _foreignDispatchReturned = AsyncTestState<bool>::create(false);
    };

    async::Task<Result<MutationExecution<std::uint8_t>>> executeRevisionOnly(
      LibraryWriteLane::Submission submission,
      library::WriteTransaction::Options options = {})
    {
      return executeInteractiveMutation(
        std::move(submission),
        [](library::LibraryWrite&) -> Result<OperationOutcome<std::uint8_t>>
        { return Changed<std::uint8_t>{.value = 1, .changeSet = {}}; },
        std::move(options));
    }

    async::Task<Result<>> abortInteractive(LibraryWriteLane::Submission submission)
    {
      auto mutationRes = co_await LibraryWriteLane::beginInteractiveMutationAsync(std::move(submission));

      if (!mutationRes)
      {
        co_return std::unexpected{mutationRes.error()};
      }

      mutationRes->abort();
      co_return Result<>{};
    }

    async::Task<AuthoringStatus> beginAndAbortAuthoring(LibraryWriteLane::Submission submission,
                                                        BoundTrackTargets targets)
    {
      auto start = co_await LibraryWriteLane::beginAuthoringMutationAsync(std::move(submission), std::move(targets));

      if (start.optMutation)
      {
        start.optMutation->abort();
      }

      co_return start.status;
    }

    async::Task<void> holdWorker(AsyncTestState<bool> ready, AsyncBarrier* release)
    {
      REQUIRE(release != nullptr);
      ready.set(true);
      release->wait();
      co_return;
    }

    async::Task<Result<MutationExecution<std::uint8_t>>> executeMaintenanceRevision(
      LibraryWriteLane::Submission enterSubmission,
      LibraryWriteLane::Submission mutationSubmission)
    {
      auto guardRes = co_await LibraryWriteLane::beginMaintenanceAsync(std::move(enterSubmission));

      if (!guardRes)
      {
        co_return std::unexpected{guardRes.error()};
      }

      auto guard = std::move(*guardRes);
      auto mutationRes = co_await LibraryWriteLane::beginMaintenanceMutationAsync(std::move(mutationSubmission), guard);

      if (!mutationRes)
      {
        auto error = mutationRes.error();
        co_await guard.finishAsync();
        co_return std::unexpected{std::move(error)};
      }

      auto executionRes =
        co_await mutationRes->executeAsync([](library::LibraryWrite&) -> Result<OperationOutcome<std::uint8_t>>
                                           { return Changed<std::uint8_t>{.value = 1, .changeSet = {}}; });
      co_await guard.finishAsync();
      co_return executionRes;
    }

    struct MutationTestEnvironment final
    {
      MutationTestEnvironment()
        : runtime{executor}
        , changes{executor, 0, "test-library"}
        , lane{runtime.callbackExecutor(),
               ao::test::requireValue(library::WritableMusicLibrary::acquire(libraryFixture.library())),
               changes}
      {
      }

      template<typename T>
      T run(async::Task<T> task)
      {
        return runQueuedTask(runtime, executor, std::move(task));
      }

      MusicLibraryFixture libraryFixture;
      QueuedExecutor executor;
      async::Runtime runtime;
      LibraryChanges changes;
      LibraryWriteLane lane;
    };
  } // namespace

  TEST_CASE("LibraryChanges - publication completes when no replica is bound", "[runtime][unit][library][changeset]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const trackId = libraryFixture.addTrack("Before");
    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto commandsFixture = LibraryCommandsFixture{libraryFixture.library(), changes};
    std::size_t appliedCount = 0;
    std::size_t notifiedCount = 0;
    auto subscription = changes.onChanged([&notifiedCount](LibraryChangeSet const&) noexcept { ++notifiedCount; });

    {
      auto replicaBinding =
        changes.bindReplica("CountingReplica", [&appliedCount](LibraryChangeSet const&) noexcept { ++appliedCount; });
      REQUIRE(commandsFixture.updateMetadata(std::array{trackId}, MetadataPatch{.optTitle = "Bound"}));
    }

    REQUIRE(commandsFixture.updateMetadata(std::array{trackId}, MetadataPatch{.optTitle = "Unbound"}));

    CHECK(appliedCount == 1);
    CHECK(notifiedCount == 2);
    CHECK(commandsFixture.library().authoringAvailability().state == LibraryAuthoringState::Available);
  }

  TEST_CASE("Library mutation execution - Changed commits its value and exact change set",
            "[runtime][unit][library][changeset]")
  {
    auto env = MutationTestEnvironment{};
    auto observed = std::vector<LibraryChangeSet>{};
    auto subscription =
      env.changes.onChanged([&observed](LibraryChangeSet const& changeSet) noexcept { observed.push_back(changeSet); });
    auto executionRes = env.run(executeInteractiveMutation(
      env.lane.captureSubmission(),
      [&env](library::LibraryWrite& write) -> Result<OperationOutcome<TrackId>>
      {
        auto const trackId = library::test::addTrackWithUniqueFixtureUri(
          env.libraryFixture.library(), write, library::test::TrackSpec{.title = "Executed"});
        return Changed<TrackId>{.value = trackId, .changeSet = LibraryChangeSet{.tracksInserted = {trackId}}};
      }));

    REQUIRE(executionRes);
    REQUIRE(executionRes->optCommittedRevision);
    CHECK(*executionRes->optCommittedRevision == 1);
    REQUIRE(executionRes->value != kInvalidTrackId);
    REQUIRE(observed.size() == 1);
    CHECK(observed.front() == (LibraryChangeSet{.libraryRevision = 1, .tracksInserted = {executionRes->value}}));
    auto read = env.libraryFixture.library().readTransaction();
    CHECK(env.libraryFixture.library()
            .tracks()
            .reader(read)
            .get(executionRes->value, library::TrackStore::Reader::LoadMode::Both)
            .has_value());
  }

  TEST_CASE("Library mutation execution - Unchanged aborts staged storage and does not publish",
            "[runtime][unit][library][changeset]")
  {
    auto env = MutationTestEnvironment{};
    std::size_t changedCount = 0;
    auto subscription = env.changes.onChanged([&changedCount](LibraryChangeSet const&) noexcept { ++changedCount; });
    auto executionRes = env.run(executeInteractiveMutation(
      env.lane.captureSubmission(),
      [&env](library::LibraryWrite& write) -> Result<OperationOutcome<TrackId>>
      {
        auto const trackId = library::test::addTrackWithUniqueFixtureUri(
          env.libraryFixture.library(), write, library::test::TrackSpec{.title = "Rolled back"});
        return Unchanged<TrackId>{.value = trackId};
      }));

    REQUIRE(executionRes);
    CHECK_FALSE(executionRes->optCommittedRevision);
    CHECK(changedCount == 0);
    auto read = env.libraryFixture.library().readTransaction();
    CHECK(env.libraryFixture.library().libraryRevision(read) == 0);
    CHECK_FALSE(env.libraryFixture.library().tracks().reader(read).get(
      executionRes->value, library::TrackStore::Reader::LoadMode::Both));
    REQUIRE(env.run(executeRevisionOnly(env.lane.captureSubmission())));
  }

  TEST_CASE("Library mutation execution - errors and exceptions release admission",
            "[runtime][unit][library][concurrency]")
  {
    auto env = MutationTestEnvironment{};

    SECTION("Result error")
    {
      auto executionRes =
        env.run(executeInteractiveMutation(env.lane.captureSubmission(),
                                           [](library::LibraryWrite&) -> Result<OperationOutcome<std::uint8_t>>
                                           { return makeError(Error::Code::Conflict, "rejected"); }));

      REQUIRE_FALSE(executionRes);
      CHECK(executionRes.error().code == Error::Code::Conflict);
      REQUIRE(env.run(executeRevisionOnly(env.lane.captureSubmission())));
    }

    SECTION("exception")
    {
      CHECK_THROWS_WITH(
        env.run(executeInteractiveMutation(env.lane.captureSubmission(),
                                           [](library::LibraryWrite&) -> Result<OperationOutcome<std::uint8_t>>
                                           { throw std::runtime_error{"execute failure"}; })),
        "execute failure");
      REQUIRE(env.run(executeRevisionOnly(env.lane.captureSubmission())));
    }

    SECTION("explicit abort")
    {
      REQUIRE(env.run(abortInteractive(env.lane.captureSubmission())));
      REQUIRE(env.run(executeRevisionOnly(env.lane.captureSubmission())));
    }
  }

  TEST_CASE("Library mutation execution - commit failure rolls back and releases admission",
            "[runtime][regression][library][changeset]")
  {
    auto env = MutationTestEnvironment{};
    auto observed = std::vector<LibraryChangeSet>{};
    auto subscription =
      env.changes.onChanged([&observed](LibraryChangeSet const& changeSet) noexcept { observed.push_back(changeSet); });
    auto stagedTrackId = kInvalidTrackId;
    auto executionRes = env.run(executeInteractiveMutation(
      env.lane.captureSubmission(),
      [&env, &stagedTrackId](library::LibraryWrite& write) -> Result<OperationOutcome<TrackId>>
      {
        stagedTrackId = library::test::addTrackWithUniqueFixtureUri(
          env.libraryFixture.library(), write, library::test::TrackSpec{.title = "Rolled back commit"});
        return Changed<TrackId>{
          .value = stagedTrackId, .changeSet = LibraryChangeSet{.tracksInserted = {stagedTrackId}}};
      },
      library::WriteTransaction::Options{
        .optInjectedCommitFailure = Error{.code = Error::Code::IoError, .message = "injected runtime commit failure"},
      },
      "Create track"));

    REQUIRE_FALSE(executionRes);
    CHECK(executionRes.error().code == Error::Code::IoError);
    CHECK(executionRes.error().message == "Create track commit failed: injected runtime commit failure");
    CHECK(stagedTrackId != kInvalidTrackId);
    CHECK(observed.empty());
    CHECK(env.lane.availability().libraryRevision == 0);
    {
      auto read = env.libraryFixture.library().readTransaction();
      CHECK(env.libraryFixture.library().libraryRevision(read) == 0);
      CHECK_FALSE(env.libraryFixture.library().tracks().reader(read).get(
        stagedTrackId, library::TrackStore::Reader::LoadMode::Both));
    }

    auto retryRes = env.run(executeRevisionOnly(env.lane.captureSubmission()));
    REQUIRE(retryRes);
    REQUIRE(retryRes->optCommittedRevision);
    CHECK(*retryRes->optCommittedRevision == 1);
    REQUIRE(observed.size() == 1);
    CHECK(observed.front().libraryRevision == 1);
  }

  TEST_CASE("LibraryChanges - applies the replica before notifying observers", "[runtime][unit][library][changeset]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const trackId = libraryFixture.addTrack("Before");
    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto commandsFixture = LibraryCommandsFixture{libraryFixture.library(), changes};
    auto phases = std::array<std::string_view, 2>{};
    std::size_t phaseCount = 0;
    auto replicaBinding = changes.bindReplica("OrderingReplica",
                                              [&phases, &phaseCount](LibraryChangeSet const&) noexcept
                                              { phases[phaseCount++] = "replica"; });
    auto observerSubscription = changes.onChanged([&phases, &phaseCount](LibraryChangeSet const&) noexcept
                                                  { phases[phaseCount++] = "observer"; });

    REQUIRE(commandsFixture.updateMetadata(std::array{trackId}, MetadataPatch{.optTitle = "After"}));

    CHECK(phaseCount == phases.size());
    CHECK(phases == std::array<std::string_view, 2>{"replica", "observer"});
  }

  TEST_CASE("LibraryChanges - only one replica may be bound at a time", "[runtime][unit][library][changeset]")
  {
    auto changes = makeStateOnlyLibraryChanges();
    auto binding = changes.bindReplica("FirstReplica", [](LibraryChangeSet const&) noexcept {});

    binding.reset();
    auto rebinding = async::Subscription{};
    CHECK_NOTHROW(rebinding = changes.bindReplica("SecondReplica", [](LibraryChangeSet const&) noexcept {}));
  }

  TEST_CASE("Library write sequencer - Closing retires an admitted publication and releases its command",
            "[runtime][regression][changeset][concurrency]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto executor = ManualExecutor{};
    auto runtime = async::Runtime{executor};
    auto changes = LibraryChanges{executor, 0, "test-library"};
    auto servicePtr = std::make_unique<LibraryWriteLane>(
      runtime.callbackExecutor(),
      ao::test::requireValue(library::WritableMusicLibrary::acquire(libraryFixture.library())),
      changes);
    auto future = runtime.spawn(executeRevisionOnly(servicePtr->captureSubmission()));

    REQUIRE(executor.waitUntilQueued());
    CHECK(executor.queuedCount() == 1);

    servicePtr.reset();
    auto const exceptionPtr = captureTaskFutureException(future);
    REQUIRE(exceptionPtr);
    CHECK(async::isOperationCancelled(exceptionPtr));
    CHECK_NOTHROW(executor.runUntilIdle());
    CHECK(executor.queuedCount() == 0);
  }

  TEST_CASE("Library write sequencer - Maintenance revision settles before workflow availability",
            "[runtime][regression][changeset][concurrency]")
  {
    auto env = MutationTestEnvironment{};
    auto phases = std::vector<std::string_view>{};
    auto changedSubscription =
      env.changes.onChanged([&phases](LibraryChangeSet const&) noexcept { phases.emplace_back("publication"); });
    auto availabilitySubscription = env.lane.onAvailabilityChanged(
      [&phases](LibraryAuthoringAvailability const& availability) noexcept
      { phases.emplace_back(availability.state == LibraryAuthoringState::Maintenance ? "maintenance" : "available"); });
    auto const enterSubmission = env.lane.captureSubmission();
    auto const mutationSubmission = env.lane.captureSubmission();

    auto executionRes = env.run(executeMaintenanceRevision(enterSubmission, mutationSubmission));

    REQUIRE(executionRes);
    REQUIRE(executionRes->optCommittedRevision);
    CHECK(phases == std::vector<std::string_view>{"maintenance", "publication", "available"});
    CHECK(env.lane.availability().state == LibraryAuthoringState::Available);
  }

  TEST_CASE("LibraryChanges - foreign publication may complete before its submission call returns",
            "[runtime][regression][changeset][concurrency]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto executor = CompletionBeforeForeignDispatchReturnsExecutor{};
    auto runtime = async::Runtime{executor, 1};
    auto changes = LibraryChanges{executor, 0, "test-library"};
    auto writeLane =
      LibraryWriteLane{runtime.callbackExecutor(),
                       ao::test::requireValue(library::WritableMusicLibrary::acquire(libraryFixture.library())),
                       changes};
    bool notified = false;
    bool notifiedBeforeSubmissionReturned = false;
    auto changedSubscription = changes.onChanged(
      [&executor, &notified, &notifiedBeforeSubmissionReturned](LibraryChangeSet const&) noexcept
      {
        notified = true;
        notifiedBeforeSubmissionReturned = !executor.foreignDispatchReturned();
      });
    auto committed = runtime.spawn(executeRevisionOnly(writeLane.captureSubmission()));

    REQUIRE(executor.waitUntilQueued());
    executor.drain();

    REQUIRE(committed.get());
    CHECK(notified);
    CHECK(notifiedBeforeSubmissionReturned);
    CHECK(writeLane.availability().libraryRevision == 1);
  }

  TEST_CASE("Library write sequencer - a second outstanding interactive command reports Busy",
            "[runtime][regression][changeset][concurrency]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto executor = ManualExecutor{};
    auto runtime = async::Runtime{executor, 1};
    auto changes = LibraryChanges{executor, 0, "test-library"};
    auto writeLane =
      LibraryWriteLane{runtime.callbackExecutor(),
                       ao::test::requireValue(library::WritableMusicLibrary::acquire(libraryFixture.library())),
                       changes};
    auto const firstSubmission = writeLane.captureSubmission();
    auto const secondSubmission = writeLane.captureSubmission();
    auto first = runtime.spawn(executeRevisionOnly(firstSubmission));

    REQUIRE(executor.waitUntilQueued());
    REQUIRE(executor.queuedCount() == 1);
    auto second = runtime.spawn(executeRevisionOnly(secondSubmission));

    auto secondRes = second.get();
    REQUIRE_FALSE(secondRes);
    CHECK(secondRes.error().code == Error::Code::ResourceBusy);

    executor.runUntilIdle();
    REQUIRE(first.get());

    auto third = runtime.spawn(executeRevisionOnly(writeLane.captureSubmission()));
    REQUIRE(executor.waitUntilQueued());
    executor.runUntilIdle();
    REQUIRE(third.get());
  }

  TEST_CASE("Library write sequencer - a later callback turn reports Busy while publication settles",
            "[runtime][regression][library-authoring][concurrency]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const trackId = libraryFixture.addTrack("Bound target");
    auto executor = ManualExecutor{};
    auto runtime = async::Runtime{executor, 1};
    auto changes = makeLibraryChanges(executor, libraryFixture.library());
    auto writeLane =
      LibraryWriteLane{runtime.callbackExecutor(),
                       ao::test::requireValue(library::WritableMusicLibrary::acquire(libraryFixture.library())),
                       changes};
    auto committed = runtime.spawn(executeRevisionOnly(writeLane.captureSubmission()));

    REQUIRE(executor.waitUntilQueued());

    auto workerReady = AsyncTestState<bool>::create(false);
    auto releaseWorker = AsyncBarrier{};
    auto workerBlocker = runtime.spawn(holdWorker(workerReady, &releaseWorker));
    REQUIRE(workerReady.waitUntil(true));

    REQUIRE(executor.runOne());
    CHECK(executor.queuedCount() == 0);

    auto const trackIds = std::array{trackId};
    auto targetsRes = writeLane.bindTrackTargets(trackIds);
    REQUIRE(targetsRes);
    CHECK(targetsRes->matches(writeLane.availability()));

    auto contenderRuntime = async::Runtime{executor, 1};
    auto contender = contenderRuntime.spawn(beginAndAbortAuthoring(writeLane.captureSubmission(), *targetsRes));

    CHECK(contender.get() == AuthoringStatus::Busy);
    CHECK(targetsRes->matches(writeLane.availability()));

    releaseWorker.release();
    workerBlocker.get();
    REQUIRE(committed.get());
  }

  TEST_CASE("LibraryChanges - writer commits publish once with the in-band revision",
            "[runtime][unit][library][changeset]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const trackId = libraryFixture.addTrack("Before");
    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto commandsFixture = LibraryCommandsFixture{libraryFixture.library(), changes};
    auto observed = std::vector<LibraryChangeSet>{};
    auto subscription =
      changes.onChanged([&observed](LibraryChangeSet const& changeSet) noexcept { observed.push_back(changeSet); });

    REQUIRE(commandsFixture.updateMetadata(std::array{trackId}, MetadataPatch{.optTitle = "After"}));
    REQUIRE(commandsFixture.updateMetadata(std::array{trackId}, MetadataPatch{.optTitle = "After"}));

    REQUIRE(observed.size() == 1);
    CHECK(observed[0].tracksMutated == std::vector{trackId});
    auto transaction = libraryFixture.library().readTransaction();
    CHECK(libraryFixture.library().libraryRevision(transaction) == observed[0].libraryRevision);
  }

  TEST_CASE("MusicLibrary - aborted write does not advance the snapshot revision", "[library][unit][revision]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    {
      auto transaction = libraryFixture.library().readTransaction();
      CHECK(libraryFixture.library().libraryRevision(transaction) == 0);
    }
    {
      auto transaction = library::test::writeTransaction(libraryFixture.library());
      CHECK(libraryFixture.library().libraryRevision(transaction) == 1);
    }
    {
      auto transaction = libraryFixture.library().readTransaction();
      CHECK(libraryFixture.library().libraryRevision(transaction) == 0);
    }
    {
      auto transaction = library::test::writeTransaction(libraryFixture.library());
      CHECK(libraryFixture.library().libraryRevision(transaction) == 1);
      REQUIRE(transaction.commit());
    }
    auto transaction = libraryFixture.library().readTransaction();
    CHECK(libraryFixture.library().libraryRevision(transaction) == 1);
  }
} // namespace ao::rt::test
