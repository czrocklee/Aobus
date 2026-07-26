// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "runtime/library/LibraryMutationService.h"
#include "test/unit/RuntimeTestSupport.h"
#include "test/unit/TestUtils.h"
#include "test/unit/library/WritableLibraryTestSupport.h"
#include <ao/Exception.h>
#include <ao/async/Subscription.h>
#include <ao/library/MusicLibrary.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryWriter.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <optional>
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

      void dispatch(std::move_only_function<void()> task) override { _delegate.dispatch(std::move(task)); }
      void defer(std::move_only_function<void()> task) override { _delegate.defer(std::move(task)); }

      void drain() { _delegate.drain(); }
      std::size_t queuedCount() const { return _delegate.queuedCount(); }
      bool waitForForeignAffinityCheck() const { return _foreignAffinityObserved.waitUntil(true); }

    private:
      QueuedExecutor _delegate;
      AsyncTestState<bool> _foreignAffinityObserved = AsyncTestState<bool>::create(false);
    };
  } // namespace

  TEST_CASE("LibraryChanges - rejects a committed revision other than the exact successor",
            "[runtime][unit][library][changeset]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto executor = ManualExecutor{};
    // The storage is at revision zero, but this deliberately claims revision
    // one was already published. The next commit is therefore not revision two.
    auto changes = LibraryChanges{executor, 1};
    auto mutationService = LibraryMutationService{
      executor, ao::test::requireValue(library::WritableMusicLibrary::acquire(libraryFixture.library())), changes};
    auto mutationResult = mutationService.beginInteractiveMutation();
    REQUIRE(mutationResult);

    CHECK_THROWS_WITH(
      mutationResult->commit(LibraryChangeSet{}),
      Catch::Matchers::ContainsSubstring("Out-of-sequence library changeset revision: expected 2, got 1"));
    CHECK(mutationService.availability().state == LibraryAuthoringState::Faulted);
    CHECK(executor.queuedCount() == 0);
  }

  TEST_CASE("LibraryChanges - publication completes when no replica is bound", "[runtime][unit][library][changeset]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const trackId = libraryFixture.addTrack("Before");
    auto changes = LibraryChanges{};
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

  TEST_CASE("LibraryChanges - only one replica may be bound at a time", "[runtime][unit][library][changeset]")
  {
    auto changes = LibraryChanges{};
    CHECK_THROWS_AS(changes.bindReplica("EmptyReplica", {}), Exception);
    auto binding = changes.bindReplica("FirstReplica", [](LibraryChangeSet const&) noexcept {});

    CHECK_THROWS_AS(changes.bindReplica("SecondReplica", [](LibraryChangeSet const&) noexcept {}), Exception);

    binding.reset();
    auto rebinding = async::Subscription{};
    CHECK_NOTHROW(rebinding = changes.bindReplica("SecondReplica", [](LibraryChangeSet const&) noexcept {}));
  }

  TEST_CASE("LibraryChanges - a replica cannot be replaced during publication",
            "[runtime][regression][library][changeset]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto executor = ManualExecutor{};
    auto changes = LibraryChanges{executor, 0};
    auto mutationService = LibraryMutationService{
      executor, ao::test::requireValue(library::WritableMusicLibrary::acquire(libraryFixture.library())), changes};
    auto binding = changes.bindReplica("FirstReplica", [](LibraryChangeSet const&) noexcept {});
    auto mutationResult = mutationService.beginInteractiveMutation();
    REQUIRE(mutationResult);
    REQUIRE(mutationResult->commit(LibraryChangeSet{}));
    CHECK(executor.queuedCount() == 1);

    binding.reset();
    CHECK_THROWS_WITH(changes.bindReplica("SecondReplica", [](LibraryChangeSet const&) noexcept {}),
                      Catch::Matchers::ContainsSubstring("during active publication"));

    executor.runUntilIdle();
    auto rebinding = async::Subscription{};
    CHECK_NOTHROW(rebinding = changes.bindReplica("SecondReplica", [](LibraryChangeSet const&) noexcept {}));
  }

  TEST_CASE("LibraryChanges - queued delivery expires with its owner", "[runtime][regression][library][changeset]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto executor = ManualExecutor{};

    {
      auto changes = LibraryChanges{executor, 0};
      auto mutationService = LibraryMutationService{
        executor, ao::test::requireValue(library::WritableMusicLibrary::acquire(libraryFixture.library())), changes};
      auto mutationResult = mutationService.beginInteractiveMutation();
      REQUIRE(mutationResult);
      REQUIRE(mutationResult->commit(LibraryChangeSet{}));
      CHECK(executor.queuedCount() == 1);
    }

    CHECK_NOTHROW(executor.runUntilIdle());
    CHECK(executor.queuedCount() == 0);
  }

  TEST_CASE("LibraryChanges - queued maintenance notification expires with its coordinator",
            "[runtime][regression][library][changeset]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto executor = ManualExecutor{};
    auto changes = LibraryChanges{};

    {
      auto mutationServicePtr = std::make_unique<LibraryMutationService>(
        executor, ao::test::requireValue(library::WritableMusicLibrary::acquire(libraryFixture.library())), changes);
      auto maintenanceResult = mutationServicePtr->beginMaintenance(LibraryMaintenanceKind::ScanApply);
      REQUIRE(maintenanceResult);
      CHECK(executor.queuedCount() == 1);
      mutationServicePtr.reset();
    }

    CHECK_NOTHROW(executor.runUntilIdle());
    CHECK(executor.queuedCount() == 0);
  }

  TEST_CASE("LibraryChanges - maintenance guard completion does not block publication completion",
            "[runtime][regression][changeset][concurrency]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto executor = AffinityProbeExecutor{};
    auto changes = LibraryChanges{executor, 0};
    auto mutationService = LibraryMutationService{
      executor, ao::test::requireValue(library::WritableMusicLibrary::acquire(libraryFixture.library())), changes};
    auto maintenanceResult = mutationService.beginMaintenance(LibraryMaintenanceKind::ScanApply);
    REQUIRE(maintenanceResult);
    executor.drain();

    auto mutationResult = mutationService.beginMaintenanceMutation(*maintenanceResult);
    REQUIRE(mutationResult);
    REQUIRE(mutationResult->commit(LibraryChangeSet{}));
    CHECK(executor.queuedCount() == 1);

    auto maintenance = std::move(*maintenanceResult);
    auto completion = std::async(
      std::launch::async,
      [optMaintenance = std::optional<LibraryMutationService::MaintenanceGuard>{std::move(maintenance)}] mutable
      { optMaintenance.reset(); });
    REQUIRE(executor.waitForForeignAffinityCheck());

    executor.drain();
    REQUIRE(completion.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
    completion.get();
    executor.drain();

    CHECK(mutationService.availability().state == LibraryAuthoringState::Available);
  }

  TEST_CASE("LibraryChanges - writer commits publish once with the in-band revision",
            "[runtime][unit][library][changeset]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const trackId = libraryFixture.addTrack("Before");
    auto changes = LibraryChanges{};
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
