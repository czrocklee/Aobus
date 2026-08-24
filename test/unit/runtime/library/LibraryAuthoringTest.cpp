// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/rt/library/LibraryAuthoring.h>

#include "runtime/library/LibraryMutationService.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/library/MusicLibraryTestSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/library/WritableLibraryTestSupport.h"
#include "test/unit/runtime/AsyncTestSupport.h"
#include "test/unit/runtime/ExecutorTestSupport.h"
#include "test/unit/runtime/RuntimeLibraryTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Runtime.h>
#include <ao/library/LibraryWrite.h>
#include <ao/library/ListStore.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/ResourceStore.h>
#include <ao/library/TrackStore.h>
#include <ao/library/WritableMusicLibrary.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryWriter.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <expected>
#include <future>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    class AuthoringFixture final
    {
    public:
      AuthoringFixture()
        : _musicLibrary{library::test::makeTestMusicLibrary(_temp.path(), _temp.path() / "db")}
        , _asyncRuntime{_executor}
      {
        _trackId =
          library::test::addTrackWithUniqueFixtureUri(_musicLibrary, library::test::TrackSpec{.title = "Before"});
        auto readTransaction = _musicLibrary.readTransaction();
        auto const revision = _musicLibrary.libraryRevision(readTransaction);
        _changesPtr = std::make_unique<LibraryChanges>(_executor, revision, "test-library");
        _libraryPtr = ao::test::requireValue(Library::create(_asyncRuntime, _musicLibrary, *_changesPtr));
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
    auto boundRes = fixture.runtimeLibrary().bindTrackTargets(std::array{fixture.trackId()});
    REQUIRE(boundRes);

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
          auto reboundRes = fixture.runtimeLibrary().bindTrackTargets(std::array{fixture.trackId()});
          reboundFromAvailability = reboundRes && reboundRes->libraryRevision() == availability.libraryRevision;
        }
      });

    auto patch = MetadataPatch{};
    patch.optTitle = "After";
    auto authoringRes = fixture.runtimeLibrary().writer().updateMetadata(*boundRes, patch);

    REQUIRE(authoringRes);
    CHECK(authoringRes->status == TrackAuthoringStatus::Applied);
    REQUIRE(authoringRes->optNextTargets);
    CHECK(authoringRes->optNextTargets->libraryRevision() == boundRes->libraryRevision() + 1U);
    CHECK(std::ranges::equal(authoringRes->optNextTargets->trackIds(), boundRes->trackIds()));
    CHECK(order == std::vector<std::string>{"replica", "change", "available"});
    CHECK(reboundFromAvailability);
    CHECK(fixture.title() == "After");
  }

  TEST_CASE("Library authoring - observer cannot reenter mutation during publication",
            "[runtime][unit][library-authoring][concurrency]")
  {
    auto fixture = AuthoringFixture{};
    auto boundRes = fixture.runtimeLibrary().bindTrackTargets(std::array{fixture.trackId()});
    REQUIRE(boundRes);
    bool nestedMutationRejected = false;
    auto changedSubscription = fixture.runtimeLibrary().changes().onChanged(
      [&](LibraryChangeSet const&) noexcept
      {
        auto nestedRes =
          fixture.runtimeLibrary().writer().createList(LibraryWriter::ListDraft{.name = "Nested mutation"});
        nestedMutationRejected = !nestedRes && nestedRes.error().code == Error::Code::InvalidState;
      });

    auto authoringRes = fixture.runtimeLibrary().writer().updateMetadata(*boundRes, MetadataPatch{.optTitle = "After"});

    REQUIRE(authoringRes);
    CHECK(authoringRes->status == TrackAuthoringStatus::Applied);
    CHECK(nestedMutationRejected);
  }

  TEST_CASE("Library authoring - retained store writers remain safely destructible after commit",
            "[runtime][unit][library-authoring]")
  {
    auto temp = ao::test::TempDir{};
    auto musicLibrary = library::test::makeTestMusicLibrary(temp.path(), temp.path() / "db");
    auto executor = InlineExecutor{};
    auto readTransaction = musicLibrary.readTransaction();
    auto changes = LibraryChanges{executor, musicLibrary.libraryRevision(readTransaction), "test-library"};
    auto writableLibrary = ao::test::requireValue(library::WritableMusicLibrary::acquire(musicLibrary));
    auto mutationService = LibraryMutationService{executor, std::move(writableLibrary), changes};
    auto mutation = ao::test::requireValue(mutationService.beginInteractiveMutation());
    auto optListWriter = std::optional<library::ListStore::Writer>{};
    REQUIRE(mutation.execute(
      [&musicLibrary, &optListWriter](library::LibraryWrite& write) -> Result<OperationOutcome<bool>>
      {
        optListWriter.emplace(library::test::physicalWriter(musicLibrary.lists(), write));
        return Changed<bool>{.value = true, .changeSet = {}};
      }));

    CHECK(optListWriter);
  }

  TEST_CASE("Library authoring - storage mutation failure unwinds the mutation scope",
            "[runtime][regression][library-authoring]")
  {
    constexpr std::size_t kMapSize = std::size_t{256} * 1024;
    auto const temp = ao::test::TempDir{};
    auto musicLibrary = ao::test::requireValue(library::MusicLibrary::open(
      temp.path(), temp.path() / "db", library::MusicLibrary::Options{.pinnedMapBytes = kMapSize}));
    auto executor = InlineExecutor{};
    auto readTransaction = musicLibrary.readTransaction();
    auto changes = LibraryChanges{executor, musicLibrary.libraryRevision(readTransaction), "test-library"};
    auto writableLibrary = ao::test::requireValue(library::WritableMusicLibrary::acquire(musicLibrary));
    auto mutationService = LibraryMutationService{executor, std::move(writableLibrary), changes};
    auto mutation = ao::test::requireValue(mutationService.beginInteractiveMutation());
    // A resource row is a fixed 36 bytes, so exhausting a pinned map takes a store
    // that still holds variable-length content; a dictionary entry is one.
    auto const oversizedText = std::string(kMapSize * 4, 'x');
    auto failureRes = mutation.apply(
      [&oversizedText](library::LibraryWrite& write) -> Result<>
      {
        auto idRes = library::test::physicalDictionary(write).intern(oversizedText);

        if (!idRes)
        {
          return std::unexpected{idRes.error()};
        }

        return {};
      });

    REQUIRE_FALSE(failureRes);
    CHECK(failureRes.error().code == Error::Code::StorageFull);

    // A failed mutation must release admission before its wrapper is destroyed.
    REQUIRE(mutationService.beginInteractiveMutation());
  }

  TEST_CASE("Library authoring - failed root operation releases coordinator admission immediately",
            "[runtime][unit][library-authoring][concurrency]")
  {
    auto const temp = ao::test::TempDir{};
    auto musicLibrary = library::test::makeTestMusicLibrary(temp.path(), temp.path() / "db");
    auto executor = InlineExecutor{};
    auto readTransaction = musicLibrary.readTransaction();
    auto changes = LibraryChanges{executor, musicLibrary.libraryRevision(readTransaction), "test-library"};
    auto writableLibrary = ao::test::requireValue(library::WritableMusicLibrary::acquire(musicLibrary));
    auto mutationService = LibraryMutationService{executor, std::move(writableLibrary), changes};

    SECTION("Result error")
    {
      auto mutation = ao::test::requireValue(mutationService.beginInteractiveMutation());
      auto failureRes =
        mutation.apply([](library::LibraryWrite&) -> Result<> { return makeError(Error::Code::Conflict, "rejected"); });

      REQUIRE_FALSE(failureRes);
      CHECK(failureRes.error().code == Error::Code::Conflict);
      REQUIRE(mutationService.beginInteractiveMutation());
    }

    SECTION("unexpected exception")
    {
      auto mutation = ao::test::requireValue(mutationService.beginInteractiveMutation());

      CHECK_THROWS_WITH(mutation.apply([](library::LibraryWrite&) -> Result<>
                                       { throw std::runtime_error{"unexpected mutation failure"}; }),
                        "unexpected mutation failure");
      REQUIRE(mutationService.beginInteractiveMutation());
    }
  }

  TEST_CASE("Library authoring - semantic no-op preserves the current binding", "[runtime][unit][library-authoring]")
  {
    auto fixture = AuthoringFixture{};
    auto boundRes = fixture.runtimeLibrary().bindTrackTargets(std::array{fixture.trackId()});
    REQUIRE(boundRes);

    std::size_t changedCount = 0;
    auto changedSubscription = fixture.runtimeLibrary().changes().onChanged(
      [&changedCount](LibraryChangeSet const&) noexcept { ++changedCount; });
    auto patch = MetadataPatch{};
    patch.optTitle = "Before";

    auto authoringRes = fixture.runtimeLibrary().writer().updateMetadata(*boundRes, patch);

    REQUIRE(authoringRes);
    CHECK(authoringRes->status == TrackAuthoringStatus::NoOp);
    CHECK_FALSE(authoringRes->optNextTargets);
    CHECK(changedCount == 0);
    CHECK(fixture.runtimeLibrary().authoringAvailability().libraryRevision == boundRes->libraryRevision());
    CHECK(fixture.title() == "Before");
  }

  TEST_CASE("Library authoring - maintenance exposes its kind and closes interactive admission",
            "[runtime][unit][library-authoring][concurrency]")
  {
    auto temp = ao::test::TempDir{};
    auto musicLibrary = library::test::makeTestMusicLibrary(temp.path(), temp.path() / "db");
    auto executor = InlineExecutor{};
    auto readTransaction = musicLibrary.readTransaction();
    auto changes = LibraryChanges{executor, musicLibrary.libraryRevision(readTransaction), "test-library"};
    auto writableLibrary = ao::test::requireValue(library::WritableMusicLibrary::acquire(musicLibrary));
    auto mutationService = LibraryMutationService{executor, std::move(writableLibrary), changes};
    auto observed = std::vector<LibraryAuthoringAvailability>{};
    bool nestedMutationRejected = false;
    auto subscription = mutationService.onAvailabilityChanged(
      [&](LibraryAuthoringAvailability const& availability) noexcept
      {
        observed.push_back(availability);

        if (availability.state == LibraryAuthoringState::Available)
        {
          auto nestedRes = mutationService.beginInteractiveMutation();
          nestedMutationRejected = !nestedRes && nestedRes.error().code == Error::Code::InvalidState;
        }
      });

    auto invalidRes = mutationService.beginMaintenance(LibraryMaintenanceKind::None);
    REQUIRE_FALSE(invalidRes);
    CHECK(invalidRes.error().code == Error::Code::InvalidInput);

    {
      auto maintenanceRes = mutationService.beginMaintenance(LibraryMaintenanceKind::Import);
      REQUIRE(maintenanceRes);
      auto maintenance = std::move(*maintenanceRes);
      auto const availability = mutationService.availability();
      CHECK(availability.state == LibraryAuthoringState::Maintenance);
      CHECK(availability.maintenanceKind == LibraryMaintenanceKind::Import);
      CHECK_FALSE(mutationService.beginInteractiveMutation());
      auto const listOrderBindingRes = mutationService.bindListOrder(kAllTracksListId, std::span<TrackId const>{});
      REQUIRE_FALSE(listOrderBindingRes);
      CHECK(listOrderBindingRes.error().code == Error::Code::InvalidState);
    }

    auto const availability = mutationService.availability();
    CHECK(availability.state == LibraryAuthoringState::Available);
    CHECK(availability.maintenanceKind == LibraryMaintenanceKind::None);
    REQUIRE(observed.size() == 2);
    CHECK(observed.front().maintenanceKind == LibraryMaintenanceKind::Import);
    CHECK(observed.back().maintenanceKind == LibraryMaintenanceKind::None);
    CHECK(nestedMutationRejected);
  }

  TEST_CASE("Library authoring - background task leases serialize and finish idempotently",
            "[runtime][unit][library-authoring][concurrency]")
  {
    auto temp = ao::test::TempDir{};
    auto musicLibrary = library::test::makeTestMusicLibrary(temp.path(), temp.path() / "db");
    auto executor = InlineExecutor{};
    auto readTransaction = musicLibrary.readTransaction();
    auto changes = LibraryChanges{executor, musicLibrary.libraryRevision(readTransaction), "test-library"};
    auto writableLibrary = ao::test::requireValue(library::WritableMusicLibrary::acquire(musicLibrary));
    auto mutationService = LibraryMutationService{executor, std::move(writableLibrary), changes};
    std::size_t availabilityCount = 0;
    auto subscription = mutationService.onAvailabilityChanged(
      [&availabilityCount](LibraryAuthoringAvailability const&) noexcept { ++availabilityCount; });

    auto backgroundRes = mutationService.beginBackgroundTask(LibraryMutationService::BackgroundTaskKind::ScanApply);
    REQUIRE(backgroundRes);
    auto background = std::move(*backgroundRes);
    CHECK(mutationService.availability().state == LibraryAuthoringState::Available);
    CHECK(availabilityCount == 0);

    auto overlappingRes =
      mutationService.beginBackgroundTask(LibraryMutationService::BackgroundTaskKind::AudioIdentityBackfill);
    REQUIRE_FALSE(overlappingRes);
    CHECK(overlappingRes.error().code == Error::Code::InvalidState);
    auto maintenanceRes = mutationService.beginMaintenance(LibraryMaintenanceKind::Import);
    REQUIRE_FALSE(maintenanceRes);
    CHECK(maintenanceRes.error().code == Error::Code::InvalidState);

    auto backgroundMutationRes = mutationService.beginBackgroundMutation(background);
    REQUIRE(backgroundMutationRes);
    backgroundMutationRes->abort();

    auto interactiveMutationRes = mutationService.beginInteractiveMutation();
    REQUIRE(interactiveMutationRes);
    interactiveMutationRes->abort();
    CHECK(availabilityCount == 0);

    background.finish();
    background.finish();
    auto staleMutationRes = mutationService.beginBackgroundMutation(background);
    REQUIRE_FALSE(staleMutationRes);
    CHECK(staleMutationRes.error().code == Error::Code::InvalidState);

    auto nextBackgroundRes =
      mutationService.beginBackgroundTask(LibraryMutationService::BackgroundTaskKind::AudioIdentityBackfill);
    REQUIRE(nextBackgroundRes);

    background.finish();
    auto overlapAfterRepeatedFinishRes =
      mutationService.beginBackgroundTask(LibraryMutationService::BackgroundTaskKind::ScanApply);
    REQUIRE_FALSE(overlapAfterRepeatedFinishRes);
    CHECK(overlapAfterRepeatedFinishRes.error().code == Error::Code::InvalidState);
  }

  TEST_CASE("Library authoring - callback owner does not wait for a background writer",
            "[runtime][regression][library-authoring][concurrency]")
  {
    auto temp = ao::test::TempDir{};
    auto musicLibrary = library::test::makeTestMusicLibrary(temp.path(), temp.path() / "db");
    auto const trackId =
      library::test::addTrackWithUniqueFixtureUri(musicLibrary, library::test::TrackSpec{.title = "Background target"});
    auto executor = InlineExecutor{};
    auto readTransaction = musicLibrary.readTransaction();
    auto changes = LibraryChanges{executor, musicLibrary.libraryRevision(readTransaction), "test-library"};
    auto writableLibrary = ao::test::requireValue(library::WritableMusicLibrary::acquire(musicLibrary));
    auto mutationService = LibraryMutationService{executor, std::move(writableLibrary), changes};
    auto targetsRes = mutationService.bindTrackTargets(std::array{trackId});
    REQUIRE(targetsRes);
    auto backgroundRes = mutationService.beginBackgroundTask(LibraryMutationService::BackgroundTaskKind::ScanApply);
    REQUIRE(backgroundRes);
    auto background = std::move(*backgroundRes);
    auto backgroundReadyPromise = std::promise<bool>{};
    auto backgroundReady = backgroundReadyPromise.get_future();
    auto releaseBackground = AsyncBarrier{};
    auto backgroundThread = std::jthread{[&]
                                         {
                                           auto mutationRes = mutationService.beginBackgroundMutation(background);
                                           backgroundReadyPromise.set_value(mutationRes.has_value());

                                           if (mutationRes)
                                           {
                                             releaseBackground.wait();
                                             mutationRes->abort();
                                           }
                                         }};

    REQUIRE(backgroundReady.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
    REQUIRE(backgroundReady.get());

    auto ownerReturnedPromise = std::promise<void>{};
    auto ownerReturned = ownerReturnedPromise.get_future();
    auto watchdogReleasedWriter = std::atomic_bool{false};
    auto watchdog = std::jthread{[&]
                                 {
                                   if (ownerReturned.wait_for(std::chrono::seconds{5}) != std::future_status::ready)
                                   {
                                     watchdogReleasedWriter.store(true, std::memory_order_relaxed);
                                     releaseBackground.release();
                                   }
                                 }};

    auto authoringStart = mutationService.beginAuthoringMutation(*targetsRes);
    ownerReturnedPromise.set_value();
    releaseBackground.release();
    backgroundThread.join();
    watchdog.join();

    CHECK(authoringStart.status == TrackAuthoringStatus::Unavailable);
    CHECK_FALSE(authoringStart.optMutation);
    CHECK_FALSE(watchdogReleasedWriter.load(std::memory_order_relaxed));

    auto afterBackground = mutationService.beginAuthoringMutation(*targetsRes);
    REQUIRE(afterBackground.optMutation);
    CHECK(afterBackground.status == TrackAuthoringStatus::NoOp);
    afterBackground.optMutation->abort();
  }

  TEST_CASE("Library authoring - foreign runtime binding is stale even during maintenance",
            "[runtime][unit][library-authoring]")
  {
    auto firstTemp = ao::test::TempDir{};
    auto firstLibrary = library::test::makeTestMusicLibrary(firstTemp.path(), firstTemp.path() / "db");
    auto const firstTrackId =
      library::test::addTrackWithUniqueFixtureUri(firstLibrary, library::test::TrackSpec{.title = "First runtime"});
    auto secondTemp = ao::test::TempDir{};
    auto secondLibrary = library::test::makeTestMusicLibrary(secondTemp.path(), secondTemp.path() / "db");
    REQUIRE(library::test::addTrackWithUniqueFixtureUri(
              secondLibrary, library::test::TrackSpec{.title = "Second runtime"}) != kInvalidTrackId);
    auto executor = InlineExecutor{};
    auto firstRead = firstLibrary.readTransaction();
    auto secondRead = secondLibrary.readTransaction();
    auto firstChanges = LibraryChanges{executor, firstLibrary.libraryRevision(firstRead), "first-test-library"};
    auto secondChanges = LibraryChanges{executor, secondLibrary.libraryRevision(secondRead), "second-test-library"};
    auto firstWritable = ao::test::requireValue(library::WritableMusicLibrary::acquire(firstLibrary));
    auto secondWritable = ao::test::requireValue(library::WritableMusicLibrary::acquire(secondLibrary));
    auto firstMutationService = LibraryMutationService{executor, std::move(firstWritable), firstChanges};
    auto secondMutationService = LibraryMutationService{executor, std::move(secondWritable), secondChanges};
    auto foreignTargets = ao::test::requireValue(firstMutationService.bindTrackTargets(std::array{firstTrackId}));
    auto maintenanceRes = secondMutationService.beginMaintenance(LibraryMaintenanceKind::Import);
    REQUIRE(maintenanceRes);
    auto maintenance = std::move(*maintenanceRes);

    auto const start = secondMutationService.beginAuthoringMutation(foreignTargets);

    CHECK(start.status == TrackAuthoringStatus::Stale);
    CHECK_FALSE(start.optMutation);
  }

  TEST_CASE("Library authoring - intervening library commit makes a target binding stale",
            "[runtime][unit][library-authoring]")
  {
    auto fixture = AuthoringFixture{};
    auto boundRes = fixture.runtimeLibrary().bindTrackTargets(std::array{fixture.trackId()});
    REQUIRE(boundRes);

    auto draft = LibraryWriter::ListDraft{
      .name = "Unrelated",
    };
    REQUIRE(fixture.runtimeLibrary().writer().createList(draft));

    auto patch = MetadataPatch{};
    patch.optTitle = "Should not apply";
    auto authoringRes = fixture.runtimeLibrary().writer().updateMetadata(*boundRes, patch);

    REQUIRE(authoringRes);
    CHECK(authoringRes->status == TrackAuthoringStatus::Stale);
    CHECK(authoringRes->reply.changes.empty());
    CHECK(fixture.title() == "Before");
  }
} // namespace ao::rt::test
