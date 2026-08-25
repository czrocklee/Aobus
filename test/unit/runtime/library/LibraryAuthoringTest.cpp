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
#include "test/unit/runtime/library/LibraryMutationTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/LoopExecutor.h>
#include <ao/async/OperationCancelled.h>
#include <ao/async/Runtime.h>
#include <ao/async/Task.h>
#include <ao/library/LibraryWrite.h>
#include <ao/library/ListStore.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/ResourceStore.h>
#include <ao/library/TrackStore.h>
#include <ao/library/WritableMusicLibrary.h>
#include <ao/rt/ListMutation.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryWriter.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <type_traits>
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

      template<typename T>
      T runTask(async::Task<T> task)
      {
        return runLoopTask(_asyncRuntime, _executor, std::move(task));
      }

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
      async::LoopExecutor _executor;
      async::Runtime _asyncRuntime;
      std::unique_ptr<LibraryChanges> _changesPtr;
      std::unique_ptr<Library> _libraryPtr;
    };

    class MutationServiceFixture final
    {
    public:
      explicit MutationServiceFixture(std::optional<library::test::TrackSpec> optInitialTrack = std::nullopt)
        : _musicLibrary{library::test::makeTestMusicLibrary(_temp.path(), _temp.path() / "db")}
        , _initialTrackId{optInitialTrack ? library::test::addTrackWithUniqueFixtureUri(_musicLibrary, *optInitialTrack)
                                          : kInvalidTrackId}
        , _asyncRuntime{_executor}
        , _changes{_executor, currentRevision(_musicLibrary), "test-library"}
        , _mutationService{_asyncRuntime.callbackExecutor(),
                           ao::test::requireValue(library::WritableMusicLibrary::acquire(_musicLibrary)),
                           _changes}
      {
      }

      template<typename T>
      T run(async::Task<T> task)
      {
        return runLoopTask(_asyncRuntime, _executor, std::move(task));
      }

      library::MusicLibrary& musicLibrary() noexcept { return _musicLibrary; }
      TrackId initialTrackId() const noexcept { return _initialTrackId; }
      async::Runtime& asyncRuntime() noexcept { return _asyncRuntime; }
      LibraryMutationService& mutationService() noexcept { return _mutationService; }

    private:
      static std::uint64_t currentRevision(library::MusicLibrary& musicLibrary)
      {
        auto const transaction = musicLibrary.readTransaction();
        return musicLibrary.libraryRevision(transaction);
      }

      ao::test::TempDir _temp;
      library::MusicLibrary _musicLibrary;
      TrackId _initialTrackId;
      async::LoopExecutor _executor;
      async::Runtime _asyncRuntime;
      LibraryChanges _changes;
      LibraryMutationService _mutationService;
    };

    template<typename Operation,
             typename OperationResult = std::remove_cvref_t<std::invoke_result_t<Operation, library::LibraryWrite&>>>
    async::Task<OperationResult> applyInteractive(LibraryMutationService::Submission submission, Operation operation)
    {
      auto mutationRes = co_await LibraryMutationService::beginInteractiveMutationAsync(std::move(submission));

      if (!mutationRes)
      {
        co_return std::unexpected{mutationRes.error()};
      }

      co_return mutationRes->apply(std::move(operation));
    }

    async::Task<Result<>> abortBackground(LibraryMutationService::Submission submission,
                                          LibraryMutationService::BackgroundTaskLease const* lease)
    {
      REQUIRE(lease != nullptr);
      auto mutationRes = co_await LibraryMutationService::beginBackgroundMutationAsync(std::move(submission), *lease);

      if (!mutationRes)
      {
        co_return std::unexpected{mutationRes.error()};
      }

      mutationRes->abort();
      co_return Result<>{};
    }

    async::Task<Result<>> abortInteractive(LibraryMutationService::Submission submission)
    {
      auto mutationRes = co_await LibraryMutationService::beginInteractiveMutationAsync(std::move(submission));

      if (!mutationRes)
      {
        co_return std::unexpected{mutationRes.error()};
      }

      mutationRes->abort();
      co_return Result<>{};
    }

    async::Task<AuthoringStatus> startAndAbortAuthoring(LibraryMutationService::Submission submission,
                                                        BoundTrackTargets targets)
    {
      auto start =
        co_await LibraryMutationService::beginAuthoringMutationAsync(std::move(submission), std::move(targets));

      if (start.optMutation)
      {
        start.optMutation->abort();
      }

      co_return start.status;
    }

    async::Task<void> holdBackgroundMutation(LibraryMutationService::Submission submission,
                                             LibraryMutationService::BackgroundTaskLease const* lease,
                                             AsyncTestState<int> ready,
                                             AsyncBarrier* release)
    {
      REQUIRE(lease != nullptr);
      REQUIRE(release != nullptr);
      auto mutationRes = co_await LibraryMutationService::beginBackgroundMutationAsync(std::move(submission), *lease);

      if (!mutationRes)
      {
        ready.set(-1);
        co_return;
      }

      ready.set(1);
      release->wait();
      mutationRes->abort();
    }
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
          reboundFromAvailability = reboundRes && reboundRes->matches(availability);
        }
      });

    auto const beforeAvailability = fixture.runtimeLibrary().authoringAvailability();
    auto patch = MetadataPatch{};
    patch.optTitle = "After";
    auto authoringRes = fixture.runTask(fixture.runtimeLibrary().writer().updateMetadata(*boundRes, patch));

    REQUIRE(authoringRes);
    CHECK(authoringRes->status == AuthoringStatus::Applied);
    REQUIRE(authoringRes->optNextTargets);
    auto const afterAvailability = fixture.runtimeLibrary().authoringAvailability();
    CHECK(afterAvailability.libraryRevision == beforeAvailability.libraryRevision + 1U);
    CHECK(authoringRes->optNextTargets->matches(afterAvailability));
    CHECK_FALSE(boundRes->matches(afterAvailability));
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
    auto optNestedTask = std::optional<async::Task<Result<ListId>>>{};
    auto changedSubscription = fixture.runtimeLibrary().changes().onChanged(
      [&](LibraryChangeSet const&) noexcept
      { optNestedTask.emplace(fixture.runtimeLibrary().writer().createList(ListDraft{.name = "Nested mutation"})); });

    auto authoringRes =
      fixture.runTask(fixture.runtimeLibrary().writer().updateMetadata(*boundRes, MetadataPatch{.optTitle = "After"}));

    REQUIRE(authoringRes);
    CHECK(authoringRes->status == AuthoringStatus::Applied);
    REQUIRE(optNestedTask);
    auto nestedRes = fixture.runTask(std::move(*optNestedTask));
    REQUIRE_FALSE(nestedRes);
    CHECK(nestedRes.error().code == Error::Code::InvalidState);
  }

  TEST_CASE("Library authoring - retained store writers remain safely destructible after commit",
            "[runtime][unit][library-authoring]")
  {
    auto env = MutationServiceFixture{};
    auto optListWriter = std::optional<library::ListStore::Writer>{};
    REQUIRE(env.run(executeInteractiveMutation(
      env.mutationService().captureSubmission(),
      [&env, &optListWriter](library::LibraryWrite& write) -> Result<OperationOutcome<bool>>
      {
        optListWriter.emplace(library::test::physicalWriter(env.musicLibrary().lists(), write));
        return Changed<bool>{.value = true, .changeSet = {}};
      })));

    CHECK(optListWriter);
  }

  TEST_CASE("Library authoring - storage mutation failure unwinds the mutation scope",
            "[runtime][regression][library-authoring]")
  {
    constexpr std::size_t kMapSize = std::size_t{256} * 1024;
    auto const temp = ao::test::TempDir{};
    auto musicLibrary = ao::test::requireValue(library::MusicLibrary::open(
      temp.path(), temp.path() / "db", library::MusicLibrary::Options{.pinnedMapBytes = kMapSize}));
    auto executor = async::LoopExecutor{};
    auto asyncRuntime = async::Runtime{executor};
    auto readTransaction = musicLibrary.readTransaction();
    auto changes = LibraryChanges{executor, musicLibrary.libraryRevision(readTransaction), "test-library"};
    auto writableLibrary = ao::test::requireValue(library::WritableMusicLibrary::acquire(musicLibrary));
    auto mutationService = LibraryMutationService{asyncRuntime.callbackExecutor(), std::move(writableLibrary), changes};
    // A resource row is a fixed 36 bytes, so exhausting a pinned map takes a store
    // that still holds variable-length content; a dictionary entry is one.
    auto const oversizedText = std::string(kMapSize * 4, 'x');
    auto failureRes = runLoopTask(asyncRuntime,
                                  executor,
                                  applyInteractive(mutationService.captureSubmission(),
                                                   [&oversizedText](library::LibraryWrite& write) -> Result<>
                                                   {
                                                     auto idRes =
                                                       library::test::physicalDictionary(write).intern(oversizedText);

                                                     if (!idRes)
                                                     {
                                                       return std::unexpected{idRes.error()};
                                                     }

                                                     return {};
                                                   }));

    REQUIRE_FALSE(failureRes);
    CHECK(failureRes.error().code == Error::Code::StorageFull);

    // A failed mutation must release admission before its wrapper is destroyed.
    auto retryRes = runLoopTask(
      asyncRuntime,
      executor,
      applyInteractive(mutationService.captureSubmission(), [](library::LibraryWrite&) -> Result<> { return {}; }));
    REQUIRE(retryRes);
  }

  TEST_CASE("Library authoring - failed root operation releases coordinator admission immediately",
            "[runtime][unit][library-authoring][concurrency]")
  {
    auto env = MutationServiceFixture{};

    SECTION("Result error")
    {
      auto failureRes = env.run(applyInteractive(env.mutationService().captureSubmission(),
                                                 [](library::LibraryWrite&) -> Result<>
                                                 { return makeError(Error::Code::Conflict, "rejected"); }));

      REQUIRE_FALSE(failureRes);
      CHECK(failureRes.error().code == Error::Code::Conflict);
      REQUIRE(env.run(applyInteractive(
        env.mutationService().captureSubmission(), [](library::LibraryWrite&) -> Result<> { return {}; })));
    }

    SECTION("unexpected exception")
    {
      CHECK_THROWS_WITH(env.run(applyInteractive(env.mutationService().captureSubmission(),
                                                 [](library::LibraryWrite&) -> Result<>
                                                 { throw std::runtime_error{"unexpected mutation failure"}; })),
                        "unexpected mutation failure");
      REQUIRE(env.run(applyInteractive(
        env.mutationService().captureSubmission(), [](library::LibraryWrite&) -> Result<> { return {}; })));
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

    auto authoringRes = fixture.runTask(fixture.runtimeLibrary().writer().updateMetadata(*boundRes, patch));

    REQUIRE(authoringRes);
    CHECK(authoringRes->status == AuthoringStatus::NoOp);
    CHECK_FALSE(authoringRes->optNextTargets);
    CHECK(changedCount == 0);
    CHECK(boundRes->matches(fixture.runtimeLibrary().authoringAvailability()));
    CHECK(fixture.title() == "Before");
  }

  TEST_CASE("Library authoring - maintenance closes interactive admission",
            "[runtime][unit][library-authoring][concurrency]")
  {
    auto env = MutationServiceFixture{};
    auto observed = std::vector<LibraryAuthoringAvailability>{};
    auto optNestedSubmission = std::optional<LibraryMutationService::Submission>{};
    auto subscription = env.mutationService().onAvailabilityChanged(
      [&](LibraryAuthoringAvailability const& availability) noexcept
      {
        observed.push_back(availability);

        if (availability.state == LibraryAuthoringState::Available)
        {
          optNestedSubmission = env.mutationService().captureSubmission();
        }
      });

    auto maintenanceRes =
      env.run(LibraryMutationService::beginMaintenanceAsync(env.mutationService().captureSubmission()));
    REQUIRE(maintenanceRes);
    auto maintenance = std::move(*maintenanceRes);
    auto const maintenanceAvailability = env.mutationService().availability();
    CHECK(maintenanceAvailability.state == LibraryAuthoringState::Maintenance);
    auto interactiveRes =
      env.run(LibraryMutationService::beginInteractiveMutationAsync(env.mutationService().captureSubmission()));
    REQUIRE_FALSE(interactiveRes);
    CHECK(interactiveRes.error().code == Error::Code::InvalidState);
    auto const listOrderBindingRes = env.mutationService().bindListOrder(kAllTracksListId, std::span<TrackId const>{});
    REQUIRE_FALSE(listOrderBindingRes);
    CHECK(listOrderBindingRes.error().code == Error::Code::InvalidState);
    env.run(maintenance.finishAsync());

    auto const availability = env.mutationService().availability();
    CHECK(availability.state == LibraryAuthoringState::Available);
    REQUIRE(observed.size() == 2);
    CHECK(observed.front().state == LibraryAuthoringState::Maintenance);
    CHECK(observed.back().state == LibraryAuthoringState::Available);
    REQUIRE(optNestedSubmission);
    auto nestedRes = env.run(LibraryMutationService::beginInteractiveMutationAsync(std::move(*optNestedSubmission)));
    REQUIRE_FALSE(nestedRes);
    CHECK(nestedRes.error().code == Error::Code::InvalidState);
  }

  TEST_CASE("Library authoring - background task leases serialize and finish idempotently",
            "[runtime][unit][library-authoring][concurrency]")
  {
    auto env = MutationServiceFixture{};
    std::size_t availabilityCount = 0;
    auto subscription = env.mutationService().onAvailabilityChanged(
      [&availabilityCount](LibraryAuthoringAvailability const&) noexcept { ++availabilityCount; });

    auto backgroundRes =
      env.mutationService().beginBackgroundTask(LibraryMutationService::BackgroundTaskKind::ScanApply);
    REQUIRE(backgroundRes);
    auto background = std::move(*backgroundRes);
    CHECK(env.mutationService().availability().state == LibraryAuthoringState::Available);
    CHECK(availabilityCount == 0);

    auto overlappingRes =
      env.mutationService().beginBackgroundTask(LibraryMutationService::BackgroundTaskKind::AudioIdentityBackfill);
    REQUIRE_FALSE(overlappingRes);
    CHECK(overlappingRes.error().code == Error::Code::ResourceBusy);
    auto maintenanceRes =
      env.run(LibraryMutationService::beginMaintenanceAsync(env.mutationService().captureSubmission()));
    REQUIRE_FALSE(maintenanceRes);
    CHECK(maintenanceRes.error().code == Error::Code::InvalidState);

    REQUIRE(env.run(abortBackground(env.mutationService().captureSubmission(), &background)));

    auto interactiveMutationRes = env.run(abortInteractive(env.mutationService().captureSubmission()));
    REQUIRE(interactiveMutationRes);
    CHECK(availabilityCount == 0);

    background.finish();
    background.finish();
    auto staleMutationRes = env.run(
      LibraryMutationService::beginBackgroundMutationAsync(env.mutationService().captureSubmission(), background));
    REQUIRE_FALSE(staleMutationRes);
    CHECK(staleMutationRes.error().code == Error::Code::InvalidState);

    auto nextBackgroundRes =
      env.mutationService().beginBackgroundTask(LibraryMutationService::BackgroundTaskKind::AudioIdentityBackfill);
    REQUIRE(nextBackgroundRes);

    background.finish();
    auto overlapAfterRepeatedFinishRes =
      env.mutationService().beginBackgroundTask(LibraryMutationService::BackgroundTaskKind::ScanApply);
    REQUIRE_FALSE(overlapAfterRepeatedFinishRes);
    CHECK(overlapAfterRepeatedFinishRes.error().code == Error::Code::ResourceBusy);
  }

  TEST_CASE("Library authoring - interactive work reports Busy behind an active background mutation",
            "[runtime][regression][library-authoring][concurrency]")
  {
    auto env = MutationServiceFixture{library::test::TrackSpec{.title = "Background target"}};
    auto targetsRes = env.mutationService().bindTrackTargets(std::array{env.initialTrackId()});
    REQUIRE(targetsRes);
    auto backgroundRes =
      env.mutationService().beginBackgroundTask(LibraryMutationService::BackgroundTaskKind::ScanApply);
    REQUIRE(backgroundRes);
    auto background = std::move(*backgroundRes);
    auto backgroundReady = AsyncTestState<int>::create(0);
    auto releaseBackground = AsyncBarrier{};
    auto backgroundFuture = env.asyncRuntime().spawn(holdBackgroundMutation(
      env.mutationService().captureSubmission(), &background, backgroundReady, &releaseBackground));
    REQUIRE(backgroundReady.waitUntil(1));

    auto const authoringStatus =
      env.run(startAndAbortAuthoring(env.mutationService().captureSubmission(), *targetsRes));

    CHECK(authoringStatus == AuthoringStatus::Busy);
    releaseBackground.release();
    CHECK_NOTHROW(backgroundFuture.get());

    auto const afterBackground =
      env.run(startAndAbortAuthoring(env.mutationService().captureSubmission(), *targetsRes));
    CHECK(afterBackground == AuthoringStatus::NoOp);
  }

  TEST_CASE("Library authoring - Closing stops active background pre-transaction work",
            "[runtime][regression][library-authoring][concurrency]")
  {
    auto temp = ao::test::TempDir{};
    auto musicLibrary = library::test::makeTestMusicLibrary(temp.path(), temp.path() / "db");
    auto executor = async::LoopExecutor{};
    auto asyncRuntime = async::Runtime{executor};
    auto readTransaction = musicLibrary.readTransaction();
    auto changes = LibraryChanges{executor, musicLibrary.libraryRevision(readTransaction), "test-library"};
    auto writableLibrary = ao::test::requireValue(library::WritableMusicLibrary::acquire(musicLibrary));
    auto mutationServicePtr =
      std::make_unique<LibraryMutationService>(asyncRuntime.callbackExecutor(), std::move(writableLibrary), changes);
    auto backgroundRes = mutationServicePtr->beginBackgroundTask(LibraryMutationService::BackgroundTaskKind::ScanApply);
    REQUIRE(backgroundRes);
    auto background = std::move(*backgroundRes);
    auto preTransactionEntered = AsyncTestState<int>::create(0);
    auto closingStopObserved = AsyncTestState<int>::create(0);
    auto releasePreTransaction = AsyncBarrier{};
    auto future = asyncRuntime.spawn(LibraryMutationService::beginBackgroundMutationAsync(
      mutationServicePtr->captureSubmission(),
      background,
      [&preTransactionEntered, &closingStopObserved, &releasePreTransaction](
        std::stop_token const stopToken) -> Result<>
      {
        auto releaseOnStop =
          std::stop_callback{stopToken, [&releasePreTransaction] { releasePreTransaction.release(); }};
        preTransactionEntered.set(1);
        releasePreTransaction.wait();
        closingStopObserved.set(stopToken.stop_requested() ? 1 : -1);
        return {};
      }));
    auto const entered = preTransactionEntered.waitUntil(1);

    if (!entered)
    {
      releasePreTransaction.release();
      mutationServicePtr.reset();
      asyncRuntime.requestStop();
      asyncRuntime.join();
    }

    REQUIRE(entered);

    mutationServicePtr.reset();

    CHECK(closingStopObserved.load() == 1);
    CHECK_THROWS_AS(std::ignore = future.get(), async::OperationCancelled);

    asyncRuntime.requestStop();
    asyncRuntime.join();
  }

  TEST_CASE("Library authoring - foreign runtime binding is stale", "[runtime][unit][library-authoring]")
  {
    auto firstTemp = ao::test::TempDir{};
    auto firstLibrary = library::test::makeTestMusicLibrary(firstTemp.path(), firstTemp.path() / "db");
    auto const firstTrackId =
      library::test::addTrackWithUniqueFixtureUri(firstLibrary, library::test::TrackSpec{.title = "First runtime"});
    auto secondTemp = ao::test::TempDir{};
    auto secondLibrary = library::test::makeTestMusicLibrary(secondTemp.path(), secondTemp.path() / "db");
    REQUIRE(library::test::addTrackWithUniqueFixtureUri(
              secondLibrary, library::test::TrackSpec{.title = "Second runtime"}) != kInvalidTrackId);
    auto executor = async::LoopExecutor{};
    auto asyncRuntime = async::Runtime{executor};
    auto firstRead = firstLibrary.readTransaction();
    auto secondRead = secondLibrary.readTransaction();
    auto firstChanges = LibraryChanges{executor, firstLibrary.libraryRevision(firstRead), "first-test-library"};
    auto secondChanges = LibraryChanges{executor, secondLibrary.libraryRevision(secondRead), "second-test-library"};
    auto firstWritable = ao::test::requireValue(library::WritableMusicLibrary::acquire(firstLibrary));
    auto secondWritable = ao::test::requireValue(library::WritableMusicLibrary::acquire(secondLibrary));
    auto firstMutationService =
      LibraryMutationService{asyncRuntime.callbackExecutor(), std::move(firstWritable), firstChanges};
    auto secondMutationService =
      LibraryMutationService{asyncRuntime.callbackExecutor(), std::move(secondWritable), secondChanges};
    auto foreignTargets = ao::test::requireValue(firstMutationService.bindTrackTargets(std::array{firstTrackId}));

    auto const start = runLoopTask(
      asyncRuntime,
      executor,
      LibraryMutationService::beginAuthoringMutationAsync(secondMutationService.captureSubmission(), foreignTargets));

    CHECK(start.status == AuthoringStatus::Stale);
    CHECK_FALSE(start.optMutation);
  }

  TEST_CASE("Library authoring - intervening library commit makes a target binding stale",
            "[runtime][unit][library-authoring]")
  {
    auto fixture = AuthoringFixture{};
    auto boundRes = fixture.runtimeLibrary().bindTrackTargets(std::array{fixture.trackId()});
    REQUIRE(boundRes);

    auto draft = ListDraft{
      .name = "Unrelated",
    };
    REQUIRE(fixture.runTask(fixture.runtimeLibrary().writer().createList(draft)));

    auto patch = MetadataPatch{};
    patch.optTitle = "Should not apply";
    auto authoringRes = fixture.runTask(fixture.runtimeLibrary().writer().updateMetadata(*boundRes, patch));

    REQUIRE(authoringRes);
    CHECK(authoringRes->status == AuthoringStatus::Stale);
    CHECK(authoringRes->reply.changes.empty());
    CHECK(fixture.title() == "Before");
  }
} // namespace ao::rt::test
