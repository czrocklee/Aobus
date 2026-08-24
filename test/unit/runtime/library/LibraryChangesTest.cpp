// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/rt/library/LibraryChanges.h>

#include "runtime/library/LibraryMutationService.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/library/WritableLibraryTestSupport.h"
#include "test/unit/runtime/AsyncTestSupport.h"
#include "test/unit/runtime/ExecutorTestSupport.h"
#include "test/unit/runtime/RuntimeLibraryTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Subscription.h>
#include <ao/compat/MoveOnlyFunction.h>
#include <ao/library/LibraryWrite.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackStore.h>
#include <ao/library/WriteTransaction.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/rt/library/LibraryWriter.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    class AffinityProbeExecutor final : public async::Executor
    {
    public:
      bool isCurrent() const noexcept override
      {
        auto const current = _delegate.isCurrent();

        if (!current)
        {
          _foreignAffinityObserved.set(true);
        }

        return current;
      }

      void dispatch(compat::MoveOnlyFunction<void()> task) override { _delegate.dispatch(std::move(task)); }
      void defer(compat::MoveOnlyFunction<void()> task) override { _delegate.defer(std::move(task)); }

      void drain() { _delegate.drain(); }
      std::size_t queuedCount() const { return _delegate.queuedCount(); }
      bool waitForForeignAffinityCheck() const { return _foreignAffinityObserved.waitUntil(true); }

    private:
      QueuedExecutor _delegate;
      AsyncTestState<bool> _foreignAffinityObserved = AsyncTestState<bool>::create(false);
    };

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

    Result<MutationExecution<std::uint8_t>> executeRevisionOnly(LibraryMutationService::Mutation& mutation)
    {
      return mutation.execute([](library::LibraryWrite&) -> Result<OperationOutcome<std::uint8_t>>
                              { return Changed<std::uint8_t>{.value = 1, .changeSet = {}}; });
    }
  } // namespace

  TEST_CASE("LibraryChanges - publication completes when no replica is bound", "[runtime][unit][library][changeset]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const trackId = libraryFixture.addTrack("Before");
    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto writerFixture = LibraryWriterFixture{libraryFixture.library(), changes};
    std::size_t appliedCount = 0;
    std::size_t notifiedCount = 0;
    auto subscription = changes.onChanged([&notifiedCount](LibraryChangeSet const&) noexcept { ++notifiedCount; });

    {
      auto replicaBinding =
        changes.bindReplica("CountingReplica", [&appliedCount](LibraryChangeSet const&) noexcept { ++appliedCount; });
      REQUIRE(writerFixture.updateMetadata(std::array{trackId}, MetadataPatch{.optTitle = "Bound"}));
    }

    REQUIRE(writerFixture.updateMetadata(std::array{trackId}, MetadataPatch{.optTitle = "Unbound"}));

    CHECK(appliedCount == 1);
    CHECK(notifiedCount == 2);
    CHECK(writerFixture.library().authoringAvailability().state == LibraryAuthoringState::Available);
  }

  TEST_CASE("Library mutation execution - Changed commits its value and exact change set",
            "[runtime][unit][library][changeset]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto executor = InlineExecutor{};
    auto changes = makeLibraryChanges(executor, libraryFixture.library());
    auto mutationService = LibraryMutationService{
      executor, ao::test::requireValue(library::WritableMusicLibrary::acquire(libraryFixture.library())), changes};
    auto observed = std::vector<LibraryChangeSet>{};
    auto subscription =
      changes.onChanged([&observed](LibraryChangeSet const& changeSet) noexcept { observed.push_back(changeSet); });
    auto mutation = ao::test::requireValue(mutationService.beginInteractiveMutation());
    auto executionRes = mutation.execute(
      [&libraryFixture](library::LibraryWrite& write) -> Result<OperationOutcome<TrackId>>
      {
        auto const trackId = library::test::addTrackWithUniqueFixtureUri(
          libraryFixture.library(), write, library::test::TrackSpec{.title = "Executed"});
        return Changed<TrackId>{.value = trackId, .changeSet = LibraryChangeSet{.tracksInserted = {trackId}}};
      });

    REQUIRE(executionRes);
    REQUIRE(executionRes->optCommittedRevision);
    CHECK(*executionRes->optCommittedRevision == 1);
    REQUIRE(executionRes->value != kInvalidTrackId);
    REQUIRE(observed.size() == 1);
    CHECK(observed.front() == (LibraryChangeSet{.libraryRevision = 1, .tracksInserted = {executionRes->value}}));
    auto read = libraryFixture.library().readTransaction();
    CHECK(libraryFixture.library()
            .tracks()
            .reader(read)
            .get(executionRes->value, library::TrackStore::Reader::LoadMode::Both)
            .has_value());
  }

  TEST_CASE("Library mutation execution - Unchanged aborts staged storage and does not publish",
            "[runtime][unit][library][changeset]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto executor = InlineExecutor{};
    auto changes = makeLibraryChanges(executor, libraryFixture.library());
    auto mutationService = LibraryMutationService{
      executor, ao::test::requireValue(library::WritableMusicLibrary::acquire(libraryFixture.library())), changes};
    std::size_t changedCount = 0;
    auto subscription = changes.onChanged([&changedCount](LibraryChangeSet const&) noexcept { ++changedCount; });
    auto mutation = ao::test::requireValue(mutationService.beginInteractiveMutation());
    auto executionRes = mutation.execute(
      [&libraryFixture](library::LibraryWrite& write) -> Result<OperationOutcome<TrackId>>
      {
        auto const trackId = library::test::addTrackWithUniqueFixtureUri(
          libraryFixture.library(), write, library::test::TrackSpec{.title = "Rolled back"});
        return Unchanged<TrackId>{.value = trackId};
      });

    REQUIRE(executionRes);
    CHECK_FALSE(executionRes->optCommittedRevision);
    CHECK(changedCount == 0);
    auto read = libraryFixture.library().readTransaction();
    CHECK(libraryFixture.library().libraryRevision(read) == 0);
    CHECK_FALSE(libraryFixture.library().tracks().reader(read).get(
      executionRes->value, library::TrackStore::Reader::LoadMode::Both));
    REQUIRE(mutationService.beginInteractiveMutation());
  }

  TEST_CASE("Library mutation execution - errors and exceptions release admission",
            "[runtime][unit][library][concurrency]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto executor = InlineExecutor{};
    auto changes = makeLibraryChanges(executor, libraryFixture.library());
    auto mutationService = LibraryMutationService{
      executor, ao::test::requireValue(library::WritableMusicLibrary::acquire(libraryFixture.library())), changes};

    SECTION("Result error")
    {
      auto mutation = ao::test::requireValue(mutationService.beginInteractiveMutation());
      auto executionRes = mutation.execute([](library::LibraryWrite&) -> Result<OperationOutcome<std::uint8_t>>
                                           { return makeError(Error::Code::Conflict, "rejected"); });

      REQUIRE_FALSE(executionRes);
      CHECK(executionRes.error().code == Error::Code::Conflict);
      REQUIRE(mutationService.beginInteractiveMutation());
    }

    SECTION("exception")
    {
      auto mutation = ao::test::requireValue(mutationService.beginInteractiveMutation());
      CHECK_THROWS_WITH(mutation.execute([](library::LibraryWrite&) -> Result<OperationOutcome<std::uint8_t>>
                                         { throw std::runtime_error{"execute failure"}; }),
                        "execute failure");
      REQUIRE(mutationService.beginInteractiveMutation());
    }

    SECTION("explicit abort")
    {
      auto mutation = ao::test::requireValue(mutationService.beginInteractiveMutation());
      mutation.abort();
      REQUIRE(mutationService.beginInteractiveMutation());
    }
  }

  TEST_CASE("Library mutation execution - commit failure rolls back and releases admission",
            "[runtime][regression][library][changeset]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto executor = InlineExecutor{};
    auto changes = makeLibraryChanges(executor, libraryFixture.library());
    auto mutationService = LibraryMutationService{
      executor, ao::test::requireValue(library::WritableMusicLibrary::acquire(libraryFixture.library())), changes};
    auto observed = std::vector<LibraryChangeSet>{};
    auto subscription =
      changes.onChanged([&observed](LibraryChangeSet const& changeSet) noexcept { observed.push_back(changeSet); });
    auto mutation = ao::test::requireValue(mutationService.beginInteractiveMutation(library::WriteTransaction::Options{
      .optInjectedCommitFailure = Error{.code = Error::Code::IoError, .message = "injected runtime commit failure"},
    }));
    auto stagedTrackId = kInvalidTrackId;
    auto executionRes = mutation.execute(
      [&libraryFixture, &stagedTrackId](library::LibraryWrite& write) -> Result<OperationOutcome<TrackId>>
      {
        stagedTrackId = library::test::addTrackWithUniqueFixtureUri(
          libraryFixture.library(), write, library::test::TrackSpec{.title = "Rolled back commit"});
        return Changed<TrackId>{
          .value = stagedTrackId, .changeSet = LibraryChangeSet{.tracksInserted = {stagedTrackId}}};
      },
      "Create track");

    REQUIRE_FALSE(executionRes);
    CHECK(executionRes.error().code == Error::Code::IoError);
    CHECK(executionRes.error().message == "Create track commit failed: injected runtime commit failure");
    CHECK(stagedTrackId != kInvalidTrackId);
    CHECK(observed.empty());
    CHECK(mutationService.availability().libraryRevision == 0);
    {
      auto read = libraryFixture.library().readTransaction();
      CHECK(libraryFixture.library().libraryRevision(read) == 0);
      CHECK_FALSE(
        libraryFixture.library().tracks().reader(read).get(stagedTrackId, library::TrackStore::Reader::LoadMode::Both));
    }

    auto retry = ao::test::requireValue(mutationService.beginInteractiveMutation());
    auto retryRes = executeRevisionOnly(retry);
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
    auto writerFixture = LibraryWriterFixture{libraryFixture.library(), changes};
    auto phases = std::array<std::string_view, 2>{};
    std::size_t phaseCount = 0;
    auto replicaBinding = changes.bindReplica("OrderingReplica",
                                              [&phases, &phaseCount](LibraryChangeSet const&) noexcept
                                              { phases[phaseCount++] = "replica"; });
    auto observerSubscription = changes.onChanged([&phases, &phaseCount](LibraryChangeSet const&) noexcept
                                                  { phases[phaseCount++] = "observer"; });

    REQUIRE(writerFixture.updateMetadata(std::array{trackId}, MetadataPatch{.optTitle = "After"}));

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

  TEST_CASE("LibraryChanges - queued delivery expires with its owner", "[runtime][regression][library][changeset]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto executor = ManualExecutor{};

    {
      auto changes = LibraryChanges{executor, 0, "test-library"};
      auto mutationService = LibraryMutationService{
        executor, ao::test::requireValue(library::WritableMusicLibrary::acquire(libraryFixture.library())), changes};
      auto mutationRes = mutationService.beginInteractiveMutation();
      REQUIRE(mutationRes);
      REQUIRE(executeRevisionOnly(*mutationRes));
      CHECK(executor.queuedCount() == 1);
    }

    CHECK_NOTHROW(executor.runUntilIdle());
    CHECK(executor.queuedCount() == 0);
  }

  TEST_CASE("LibraryChanges - queued maintenance finalization expires with its coordinator",
            "[runtime][regression][library][changeset]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto executor = AffinityProbeExecutor{};
    auto changes = LibraryChanges{executor, 0, "test-library"};
    auto mutationServicePtr = std::make_unique<LibraryMutationService>(
      executor, ao::test::requireValue(library::WritableMusicLibrary::acquire(libraryFixture.library())), changes);
    auto maintenanceRes = mutationServicePtr->beginMaintenance(LibraryMaintenanceKind::Import);
    REQUIRE(maintenanceRes);
    auto maintenance = std::move(*maintenanceRes);

    auto completion = std::async(
      std::launch::async,
      [optMaintenance = std::optional<LibraryMutationService::MaintenanceGuard>{std::move(maintenance)}] mutable
      { optMaintenance.reset(); });
    REQUIRE(executor.waitForForeignAffinityCheck());
    completion.get();
    CHECK(executor.queuedCount() == 1);

    mutationServicePtr.reset();

    CHECK_NOTHROW(executor.drain());
    CHECK(executor.queuedCount() == 0);
  }

  TEST_CASE("LibraryChanges - foreign publication precedes maintenance finalization",
            "[runtime][regression][changeset][concurrency]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto executor = AffinityProbeExecutor{};
    auto changes = LibraryChanges{executor, 0, "test-library"};
    auto mutationService = LibraryMutationService{
      executor, ao::test::requireValue(library::WritableMusicLibrary::acquire(libraryFixture.library())), changes};
    auto maintenanceRes = mutationService.beginMaintenance(LibraryMaintenanceKind::Import);
    REQUIRE(maintenanceRes);
    auto phases = std::vector<std::string_view>{};
    auto changedSubscription =
      changes.onChanged([&phases](LibraryChangeSet const&) noexcept { phases.emplace_back("publication"); });
    auto availabilitySubscription = mutationService.onAvailabilityChanged(
      [&phases](LibraryAuthoringAvailability const& availability) noexcept
      {
        if (availability.state == LibraryAuthoringState::Available)
        {
          phases.emplace_back("maintenance-finalization");
        }
      });

    auto mutationRes = mutationService.beginMaintenanceMutation(*maintenanceRes);
    REQUIRE(mutationRes);
    REQUIRE(executeRevisionOnly(*mutationRes));
    CHECK(executor.queuedCount() == 1);

    auto maintenance = std::move(*maintenanceRes);
    auto completion = std::async(
      std::launch::async,
      [optMaintenance = std::optional<LibraryMutationService::MaintenanceGuard>{std::move(maintenance)}] mutable
      { optMaintenance.reset(); });
    REQUIRE(executor.waitForForeignAffinityCheck());
    REQUIRE(completion.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
    completion.get();
    REQUIRE(executor.queuedCount() == 2);
    executor.drain();

    CHECK(phases == std::vector<std::string_view>{"publication", "maintenance-finalization"});
    CHECK(mutationService.availability().state == LibraryAuthoringState::Available);
  }

  TEST_CASE("LibraryChanges - foreign publication may complete before its submission call returns",
            "[runtime][regression][changeset][concurrency]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto executor = CompletionBeforeForeignDispatchReturnsExecutor{};
    auto changes = LibraryChanges{executor, 0, "test-library"};
    auto mutationService = LibraryMutationService{
      executor, ao::test::requireValue(library::WritableMusicLibrary::acquire(libraryFixture.library())), changes};
    bool notified = false;
    bool notifiedBeforeSubmissionReturned = false;
    auto changedSubscription = changes.onChanged(
      [&executor, &notified, &notifiedBeforeSubmissionReturned](LibraryChangeSet const&) noexcept
      {
        notified = true;
        notifiedBeforeSubmissionReturned = !executor.foreignDispatchReturned();
      });
    auto committed = std::async(std::launch::async,
                                [&mutationService]
                                {
                                  auto mutationRes = mutationService.beginInteractiveMutation();
                                  return mutationRes && executeRevisionOnly(*mutationRes).has_value();
                                });

    REQUIRE(executor.waitUntilQueued());
    executor.drain();

    REQUIRE(committed.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
    CHECK(committed.get());
    CHECK(notified);
    CHECK(notifiedBeforeSubmissionReturned);
    CHECK(mutationService.availability().libraryRevision == 1);
    CHECK(mutationService.beginInteractiveMutation());
  }

  TEST_CASE("LibraryChanges - next foreign writer waits for publication completion",
            "[runtime][regression][changeset][concurrency]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto executor = QueuedExecutor{};
    auto changes = LibraryChanges{executor, 0, "test-library"};
    auto mutationService = LibraryMutationService{
      executor, ao::test::requireValue(library::WritableMusicLibrary::acquire(libraryFixture.library())), changes};
    auto firstMutation = ao::test::requireValue(mutationService.beginInteractiveMutation());
    REQUIRE(executeRevisionOnly(firstMutation));
    REQUIRE(executor.queuedCount() == 1);

    auto started = AsyncTestState<bool>::create(false);
    auto nextWriter = std::async(std::launch::async,
                                 [&mutationService, started]
                                 {
                                   started.set(true);
                                   auto mutationRes = mutationService.beginInteractiveMutation();
                                   return mutationRes.has_value();
                                 });
    REQUIRE(started.waitUntil(true));
    CHECK(nextWriter.wait_for(std::chrono::milliseconds{20}) == std::future_status::timeout);

    executor.drain();

    REQUIRE(nextWriter.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
    REQUIRE(nextWriter.get());
  }

  TEST_CASE("LibraryChanges - writer commits publish once with the in-band revision",
            "[runtime][unit][library][changeset]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const trackId = libraryFixture.addTrack("Before");
    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto writerFixture = LibraryWriterFixture{libraryFixture.library(), changes};
    auto observed = std::vector<LibraryChangeSet>{};
    auto subscription =
      changes.onChanged([&observed](LibraryChangeSet const& changeSet) noexcept { observed.push_back(changeSet); });

    REQUIRE(writerFixture.updateMetadata(std::array{trackId}, MetadataPatch{.optTitle = "After"}));
    REQUIRE(writerFixture.updateMetadata(std::array{trackId}, MetadataPatch{.optTitle = "After"}));

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
