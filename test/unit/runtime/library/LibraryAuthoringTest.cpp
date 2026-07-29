// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "runtime/library/LibraryMutationService.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/library/MusicLibraryTestSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/runtime/ExecutorTestSupport.h"
#include "test/unit/runtime/RuntimeLibraryTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/Exception.h>
#include <ao/async/Runtime.h>
#include <ao/library/ListStore.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackStore.h>
#include <ao/library/WritableMusicLibrary.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryWriter.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    class RejectingExecutor final : public async::Executor
    {
    public:
      bool isCurrent() const noexcept override { return true; }
      void dispatch(std::move_only_function<void()> /*task*/) override
      {
        throwException<Exception>("Executor rejected dispatch");
      }
      void defer(std::move_only_function<void()> /*task*/) override
      {
        throwException<Exception>("Executor rejected defer");
      }
    };

    class AuthoringFixture final
    {
    public:
      AuthoringFixture()
        : _musicLibrary{library::test::makeTestMusicLibrary(_temp.path(), _temp.path() / "db")}
        , _asyncRuntime{_executor}
      {
        _trackId = library::test::addTrack(_musicLibrary, library::test::TrackSpec{.title = "Before"});
        auto readTransaction = _musicLibrary.readTransaction();
        auto const revision = _musicLibrary.libraryRevision(readTransaction);
        _changesPtr = std::make_unique<LibraryChanges>(_executor, revision);
        _libraryPtr = std::make_unique<Library>(_asyncRuntime, _musicLibrary, *_changesPtr);
      }

      ~AuthoringFixture()
      {
        _libraryPtr.reset();
        _changesPtr.reset();
        _asyncRuntime.requestStop();
        _asyncRuntime.join();
      }

      AuthoringFixture(AuthoringFixture const&) = delete;
      AuthoringFixture& operator=(AuthoringFixture const&) = delete;
      AuthoringFixture(AuthoringFixture&&) = delete;
      AuthoringFixture& operator=(AuthoringFixture&&) = delete;

      Library& runtimeLibrary() const { return *_libraryPtr; }
      TrackId trackId() const noexcept { return _trackId; }

      std::string title() const
      {
        auto transaction = _musicLibrary.readTransaction();
        auto const optView =
          _musicLibrary.tracks().reader(transaction).get(_trackId, library::TrackStore::Reader::LoadMode::Hot);
        REQUIRE(optView);
        return std::string{optView->metadata().title()};
      }

    private:
      ao::test::TempDir _temp;
      library::MusicLibrary _musicLibrary;
      TrackId _trackId = kInvalidTrackId;
      InlineExecutor _executor;
      async::Runtime _asyncRuntime;
      std::unique_ptr<LibraryChanges> _changesPtr;
      std::unique_ptr<Library> _libraryPtr;
    };
  } // namespace

  TEST_CASE("Library authoring - applied change updates the replica before notifications and the next binding",
            "[runtime][unit][library-authoring]")
  {
    auto fixture = AuthoringFixture{};
    auto boundResult = fixture.runtimeLibrary().bindTrackTargets(std::array{fixture.trackId()});
    REQUIRE(boundResult);

    auto order = std::vector<std::string>{};
    bool reboundFromAvailability = false;
    auto replicaBinding = fixture.runtimeLibrary().changes().bindReplica(
      "TestProjectionReplica", [&order](LibraryChangeSet const&) noexcept { order.emplace_back("replica"); });
    auto changedSubscription = fixture.runtimeLibrary().changes().onChanged([&order](LibraryChangeSet const&) noexcept
                                                                            { order.emplace_back("change"); });
    auto availabilitySubscription = fixture.runtimeLibrary().onAuthoringAvailabilityChanged(
      [&fixture, &order, &reboundFromAvailability](LibraryAuthoringAvailability const& availability) noexcept
      {
        if (availability.state == LibraryAuthoringState::Available)
        {
          order.emplace_back("available");
          auto reboundResult = fixture.runtimeLibrary().bindTrackTargets(std::array{fixture.trackId()});
          reboundFromAvailability = reboundResult && reboundResult->libraryRevision() == availability.libraryRevision;
        }
      });

    auto patch = MetadataPatch{};
    patch.optTitle = "After";
    auto authoringResult = fixture.runtimeLibrary().writer().updateMetadata(*boundResult, patch);

    REQUIRE(authoringResult);
    CHECK(authoringResult->status == TrackAuthoringStatus::Applied);
    REQUIRE(authoringResult->optNextTargets);
    CHECK(authoringResult->optNextTargets->libraryRevision() == boundResult->libraryRevision() + 1U);
    CHECK(std::ranges::equal(authoringResult->optNextTargets->trackIds(), boundResult->trackIds()));
    CHECK(order == std::vector<std::string>{"replica", "change", "available"});
    CHECK(reboundFromAvailability);
    CHECK(fixture.title() == "After");
  }

  TEST_CASE("Library authoring - observer cannot reenter mutation during publication",
            "[runtime][unit][library-authoring][concurrency]")
  {
    auto fixture = AuthoringFixture{};
    auto boundResult = fixture.runtimeLibrary().bindTrackTargets(std::array{fixture.trackId()});
    REQUIRE(boundResult);
    bool nestedMutationRejected = false;
    auto changedSubscription = fixture.runtimeLibrary().changes().onChanged(
      [&](LibraryChangeSet const&) noexcept
      {
        auto nestedResult =
          fixture.runtimeLibrary().writer().createList(LibraryWriter::ListDraft{.name = "Nested mutation"});
        nestedMutationRejected = !nestedResult && nestedResult.error().code == Error::Code::InvalidState;
      });

    auto authoringResult =
      fixture.runtimeLibrary().writer().updateMetadata(*boundResult, MetadataPatch{.optTitle = "After"});

    REQUIRE(authoringResult);
    CHECK(authoringResult->status == TrackAuthoringStatus::Applied);
    CHECK(nestedMutationRejected);
  }

  TEST_CASE("Library authoring - commit retains the transaction wrapper for existing store writers",
            "[runtime][unit][library-authoring]")
  {
    auto temp = ao::test::TempDir{};
    auto musicLibrary = library::test::makeTestMusicLibrary(temp.path(), temp.path() / "db");
    auto executor = InlineExecutor{};
    auto readTransaction = musicLibrary.readTransaction();
    auto changes = LibraryChanges{executor, musicLibrary.libraryRevision(readTransaction)};
    auto writableLibrary = ao::test::requireValue(library::WritableMusicLibrary::acquire(musicLibrary));
    auto mutationService = LibraryMutationService{executor, std::move(writableLibrary), changes};
    auto mutation = ao::test::requireValue(mutationService.beginInteractiveMutation());
    auto listWriter = musicLibrary.lists().writer(mutation.transaction());

    REQUIRE(mutation.commit(LibraryChangeSet{}));

    CHECK_THROWS_AS(listWriter.get(ListId{1}), Exception);
  }

  TEST_CASE("Library authoring - semantic no-op preserves the current binding", "[runtime][unit][library-authoring]")
  {
    auto fixture = AuthoringFixture{};
    auto boundResult = fixture.runtimeLibrary().bindTrackTargets(std::array{fixture.trackId()});
    REQUIRE(boundResult);

    std::size_t changedCount = 0;
    auto changedSubscription = fixture.runtimeLibrary().changes().onChanged(
      [&changedCount](LibraryChangeSet const&) noexcept { ++changedCount; });
    auto patch = MetadataPatch{};
    patch.optTitle = "Before";

    auto authoringResult = fixture.runtimeLibrary().writer().updateMetadata(*boundResult, patch);

    REQUIRE(authoringResult);
    CHECK(authoringResult->status == TrackAuthoringStatus::NoOp);
    CHECK_FALSE(authoringResult->optNextTargets);
    CHECK(changedCount == 0);
    CHECK(fixture.runtimeLibrary().authoringAvailability().libraryRevision == boundResult->libraryRevision());
    CHECK(fixture.title() == "Before");
  }

  TEST_CASE("Library authoring - maintenance exposes its kind and closes interactive admission",
            "[runtime][unit][library-authoring][concurrency]")
  {
    auto temp = ao::test::TempDir{};
    auto musicLibrary = library::test::makeTestMusicLibrary(temp.path(), temp.path() / "db");
    auto executor = InlineExecutor{};
    auto readTransaction = musicLibrary.readTransaction();
    auto changes = LibraryChanges{executor, musicLibrary.libraryRevision(readTransaction)};
    auto writableLibrary = ao::test::requireValue(library::WritableMusicLibrary::acquire(musicLibrary));
    auto mutationService = LibraryMutationService{executor, std::move(writableLibrary), changes};
    auto observed = std::vector<LibraryAuthoringAvailability>{};
    auto subscription = mutationService.onAvailabilityChanged(
      [&observed](LibraryAuthoringAvailability const& availability) noexcept { observed.push_back(availability); });

    auto invalidResult = mutationService.beginMaintenance(LibraryMaintenanceKind::None);
    REQUIRE_FALSE(invalidResult);
    CHECK(invalidResult.error().code == Error::Code::InvalidInput);

    {
      auto maintenanceResult = mutationService.beginMaintenance(LibraryMaintenanceKind::ScanApply);
      REQUIRE(maintenanceResult);
      auto maintenance = std::move(*maintenanceResult);
      auto const availability = mutationService.availability();
      CHECK(availability.state == LibraryAuthoringState::Maintenance);
      CHECK(availability.maintenanceKind == LibraryMaintenanceKind::ScanApply);
      CHECK_FALSE(mutationService.beginInteractiveMutation());
      auto const listOrderBinding = mutationService.bindListOrder(kAllTracksListId, std::span<TrackId const>{});
      REQUIRE_FALSE(listOrderBinding);
      CHECK(listOrderBinding.error().code == Error::Code::InvalidState);
    }

    auto const availability = mutationService.availability();
    CHECK(availability.state == LibraryAuthoringState::Available);
    CHECK(availability.maintenanceKind == LibraryMaintenanceKind::None);
    REQUIRE(observed.size() == 2);
    CHECK(observed.front().maintenanceKind == LibraryMaintenanceKind::ScanApply);
    CHECK(observed.back().maintenanceKind == LibraryMaintenanceKind::None);
  }

  TEST_CASE("Library authoring - foreign runtime binding is stale even during maintenance",
            "[runtime][unit][library-authoring]")
  {
    auto firstTemp = ao::test::TempDir{};
    auto firstLibrary = library::test::makeTestMusicLibrary(firstTemp.path(), firstTemp.path() / "db");
    auto const firstTrackId = library::test::addTrack(firstLibrary, library::test::TrackSpec{.title = "First runtime"});
    auto secondTemp = ao::test::TempDir{};
    auto secondLibrary = library::test::makeTestMusicLibrary(secondTemp.path(), secondTemp.path() / "db");
    REQUIRE(library::test::addTrack(secondLibrary, library::test::TrackSpec{.title = "Second runtime"}) !=
            kInvalidTrackId);
    auto executor = InlineExecutor{};
    auto firstRead = firstLibrary.readTransaction();
    auto secondRead = secondLibrary.readTransaction();
    auto firstChanges = LibraryChanges{executor, firstLibrary.libraryRevision(firstRead)};
    auto secondChanges = LibraryChanges{executor, secondLibrary.libraryRevision(secondRead)};
    auto firstWritable = ao::test::requireValue(library::WritableMusicLibrary::acquire(firstLibrary));
    auto secondWritable = ao::test::requireValue(library::WritableMusicLibrary::acquire(secondLibrary));
    auto firstMutationService = LibraryMutationService{executor, std::move(firstWritable), firstChanges};
    auto secondMutationService = LibraryMutationService{executor, std::move(secondWritable), secondChanges};
    auto foreignTargets = ao::test::requireValue(firstMutationService.bindTrackTargets(std::array{firstTrackId}));
    auto maintenanceResult = secondMutationService.beginMaintenance(LibraryMaintenanceKind::ScanApply);
    REQUIRE(maintenanceResult);
    auto maintenance = std::move(*maintenanceResult);

    auto const start = secondMutationService.beginAuthoringMutation(foreignTargets);

    CHECK(start.status == TrackAuthoringStatus::Stale);
    CHECK_FALSE(start.optMutation);
  }

  TEST_CASE("Library authoring - intervening library commit makes a target binding stale",
            "[runtime][unit][library-authoring]")
  {
    auto fixture = AuthoringFixture{};
    auto boundResult = fixture.runtimeLibrary().bindTrackTargets(std::array{fixture.trackId()});
    REQUIRE(boundResult);

    auto draft = LibraryWriter::ListDraft{
      .name = "Unrelated",
    };
    REQUIRE(fixture.runtimeLibrary().writer().createList(draft));

    auto patch = MetadataPatch{};
    patch.optTitle = "Should not apply";
    auto authoringResult = fixture.runtimeLibrary().writer().updateMetadata(*boundResult, patch);

    REQUIRE(authoringResult);
    CHECK(authoringResult->status == TrackAuthoringStatus::Stale);
    CHECK(authoringResult->reply.changes.empty());
    CHECK(fixture.title() == "Before");
  }

  TEST_CASE("Library authoring - publication enqueue failure after commit faults the mutationService",
            "[runtime][unit][library-authoring]")
  {
    auto temp = ao::test::TempDir{};
    auto musicLibrary = library::test::makeTestMusicLibrary(temp.path(), temp.path() / "db");
    auto initialRead = musicLibrary.readTransaction();
    auto const initialRevision = musicLibrary.libraryRevision(initialRead);
    auto executor = RejectingExecutor{};
    auto changes = LibraryChanges{executor, initialRevision};
    auto writableLibrary = ao::test::requireValue(library::WritableMusicLibrary::acquire(musicLibrary));
    auto mutationService = LibraryMutationService{executor, std::move(writableLibrary), changes};
    auto mutation = ao::test::requireValue(mutationService.beginInteractiveMutation());

    CHECK_THROWS_AS(mutation.commit(LibraryChangeSet{}), Exception);

    auto committedRead = musicLibrary.readTransaction();
    CHECK(musicLibrary.libraryRevision(committedRead) == initialRevision + 1U);
    CHECK(mutationService.availability().state == LibraryAuthoringState::Faulted);
    CHECK_FALSE(mutationService.beginInteractiveMutation());
  }

  TEST_CASE("Library authoring - publication validation failure after commit faults the mutationService",
            "[runtime][unit][library-authoring]")
  {
    auto temp = ao::test::TempDir{};
    auto musicLibrary = library::test::makeTestMusicLibrary(temp.path(), temp.path() / "db");
    auto initialRead = musicLibrary.readTransaction();
    auto const initialRevision = musicLibrary.libraryRevision(initialRead);
    auto executor = InlineExecutor{};
    auto changes = LibraryChanges{executor, initialRevision + 1U};
    auto writableLibrary = ao::test::requireValue(library::WritableMusicLibrary::acquire(musicLibrary));
    auto mutationService = LibraryMutationService{executor, std::move(writableLibrary), changes};
    auto mutation = ao::test::requireValue(mutationService.beginInteractiveMutation());

    CHECK_THROWS_AS(mutation.commit(LibraryChangeSet{}), Exception);

    auto committedRead = musicLibrary.readTransaction();
    CHECK(musicLibrary.libraryRevision(committedRead) == initialRevision + 1U);
    CHECK(mutationService.availability().state == LibraryAuthoringState::Faulted);
    CHECK_FALSE(mutationService.beginInteractiveMutation());
  }

  TEST_CASE("Library authoring - worker publication failure reports availability on the callback executor",
            "[runtime][unit][library-authoring][concurrency]")
  {
    auto temp = ao::test::TempDir{};
    auto musicLibrary = library::test::makeTestMusicLibrary(temp.path(), temp.path() / "db");
    auto initialRead = musicLibrary.readTransaction();
    auto const initialRevision = musicLibrary.libraryRevision(initialRead);
    auto executor = QueuedExecutor{};
    auto changes = LibraryChanges{executor, initialRevision + 1U};
    auto writableLibrary = ao::test::requireValue(library::WritableMusicLibrary::acquire(musicLibrary));
    auto mutationService = LibraryMutationService{executor, std::move(writableLibrary), changes};
    auto observed = std::vector<LibraryAuthoringAvailability>{};
    auto subscription = mutationService.onAvailabilityChanged(
      [&observed](LibraryAuthoringAvailability const& availability) noexcept { observed.push_back(availability); });

    auto commitFuture = std::async(std::launch::async,
                                   [&mutationService]
                                   {
                                     auto mutationResult = mutationService.beginInteractiveMutation();

                                     if (!mutationResult)
                                     {
                                       return false;
                                     }

                                     auto mutation = std::move(*mutationResult);

                                     try
                                     {
                                       std::ignore = mutation.commit(LibraryChangeSet{});
                                     }
                                     catch (Exception const&)
                                     {
                                       return true;
                                     }

                                     return false;
                                   });

    CHECK(commitFuture.get());
    CHECK(observed.empty());
    CHECK(mutationService.availability().state == LibraryAuthoringState::Faulted);
    CHECK(executor.queuedCount() == 1);

    executor.drain();

    REQUIRE(observed.size() == 1);
    CHECK(observed.front().state == LibraryAuthoringState::Faulted);
  }

  TEST_CASE("Library authoring - maintenance dispatch failure faults the mutationService",
            "[runtime][unit][library-authoring]")
  {
    auto temp = ao::test::TempDir{};
    auto musicLibrary = library::test::makeTestMusicLibrary(temp.path(), temp.path() / "db");
    auto changes = makeInlineLibraryChanges();
    auto executor = RejectingExecutor{};
    auto writableLibrary = ao::test::requireValue(library::WritableMusicLibrary::acquire(musicLibrary));
    auto mutationService = LibraryMutationService{executor, std::move(writableLibrary), changes};

    CHECK_THROWS_AS(mutationService.beginMaintenance(LibraryMaintenanceKind::ScanApply), Exception);
    CHECK(mutationService.availability().state == LibraryAuthoringState::Faulted);
    CHECK(mutationService.availability().maintenanceKind == LibraryMaintenanceKind::None);
    CHECK_FALSE(mutationService.beginInteractiveMutation());
  }
} // namespace ao::rt::test
