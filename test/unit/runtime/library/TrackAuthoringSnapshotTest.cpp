// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "runtime/library/LibraryWriteLane.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/runtime/RuntimeLibraryTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/LoopExecutor.h>
#include <ao/library/WritableMusicLibrary.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/rt/library/LibraryCommands.h>
#include <ao/rt/library/LibrarySnapshot.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <span>
#include <utility>

namespace ao::rt::test
{
  TEST_CASE("Library snapshot binding - owns ordered duplicate targets at the baseline revision",
            "[runtime][unit][library-authoring]")
  {
    auto storage = MusicLibraryFixture{};
    auto const first = storage.addTrack(library::test::TrackSpec{.title = "First"});
    auto const second = storage.addTrack(library::test::TrackSpec{.title = "Second"});
    auto executor = async::LoopExecutor{};
    auto changes = makeLibraryChanges(executor, storage.library());
    auto fixture = LibraryCommandsFixture{storage.library(), changes, executor};
    auto const targetIds = std::array{second, first, second};

    auto prepared = [&]
    {
      auto snapshot = fixture.library().snapshot();
      auto boundRes = fixture.library().bindTrackTargets(targetIds, snapshot);
      REQUIRE(boundRes);
      CHECK(boundRes->revision() == snapshot.revision());
      CHECK(boundRes->matches(fixture.library().authoringAvailability()));
      CHECK(std::ranges::equal(boundRes->trackIds(), targetIds));
      auto const legacyRes = fixture.library().bindTrackTargets(targetIds);
      REQUIRE(legacyRes);
      CHECK(*legacyRes == *boundRes);
      auto optRow = snapshot.trackRow(second);
      REQUIRE(optRow);
      CHECK(optRow->title == "Second");
      return std::pair{std::move(*boundRes), std::move(*optRow)};
    }();

    // Neither the binding nor the baseline borrows the released read transaction.
    CHECK(std::ranges::equal(prepared.first.trackIds(), targetIds));
    CHECK(prepared.second.id == second);
    CHECK(prepared.second.title == "Second");
    CHECK(prepared.first.matches(fixture.library().authoringAvailability()));
  }

  TEST_CASE("Library snapshot binding - rejects empty invalid and missing targets as a complete set",
            "[runtime][unit][library-authoring]")
  {
    auto storage = MusicLibraryFixture{};
    auto const trackId = storage.addTrack(library::test::TrackSpec{.title = "Unchanged"});
    auto executor = async::LoopExecutor{};
    auto changes = makeLibraryChanges(executor, storage.library());
    auto fixture = LibraryCommandsFixture{storage.library(), changes, executor};
    auto snapshot = fixture.library().snapshot();

    SECTION("empty target sequence")
    {
      auto const boundRes = fixture.library().bindTrackTargets(std::span<TrackId const>{}, snapshot);
      REQUIRE_FALSE(boundRes);
      CHECK(boundRes.error().code == Error::Code::InvalidInput);
    }

    SECTION("invalid id after an existing target")
    {
      auto const boundRes = fixture.library().bindTrackTargets(std::array{trackId, kInvalidTrackId}, snapshot);
      REQUIRE_FALSE(boundRes);
      CHECK(boundRes.error().code == Error::Code::NotFound);
    }

    SECTION("missing id after an existing target")
    {
      auto const missing = TrackId{99999};
      REQUIRE_FALSE(snapshot.containsTrack(missing));
      auto const boundRes = fixture.library().bindTrackTargets(std::array{trackId, missing}, snapshot);
      REQUIRE_FALSE(boundRes);
      CHECK(boundRes.error().code == Error::Code::NotFound);
    }

    CHECK(snapshot.revision() == fixture.library().authoringAvailability().libraryRevision);
    auto const optRow = snapshot.trackRow(trackId);
    REQUIRE(optRow);
    CHECK(optRow->title == "Unchanged");
  }

  TEST_CASE("Library snapshot binding - rejects foreign storage despite identical ids and revisions",
            "[runtime][unit][library-authoring]")
  {
    auto storage = MusicLibraryFixture{};
    auto foreignStorage = MusicLibraryFixture{};
    auto const trackId = storage.addTrack(library::test::TrackSpec{.title = "Local"});
    auto const foreignId = foreignStorage.addTrack(library::test::TrackSpec{.title = "Foreign"});
    auto executor = async::LoopExecutor{};
    auto changes = makeLibraryChanges(executor, storage.library());
    auto foreignChanges = makeLibraryChanges(executor, foreignStorage.library());
    auto fixture = LibraryCommandsFixture{storage.library(), changes, executor};
    auto foreignFixture = LibraryCommandsFixture{foreignStorage.library(), foreignChanges, executor};
    auto foreignSnapshot = foreignFixture.library().snapshot();
    REQUIRE(trackId == foreignId);
    REQUIRE(foreignSnapshot.revision() == fixture.library().authoringAvailability().libraryRevision);
    REQUIRE(foreignSnapshot.containsTrack(trackId));

    auto const boundRes = fixture.library().bindTrackTargets(std::array{trackId}, foreignSnapshot);

    REQUIRE_FALSE(boundRes);
    CHECK(boundRes.error().code == Error::Code::InvalidInput);
    auto const optLocalRow = fixture.library().snapshot().trackRow(trackId);
    REQUIRE(optLocalRow);
    CHECK(optLocalRow->title == "Local");
  }

  TEST_CASE("Library snapshot binding - retained old baseline cannot rebind or submit after a commit",
            "[runtime][unit][library-authoring]")
  {
    auto storage = MusicLibraryFixture{};
    auto const trackId = storage.addTrack(library::test::TrackSpec{.title = "Before"});
    auto executor = async::LoopExecutor{};
    auto changes = makeLibraryChanges(executor, storage.library());
    auto fixture = LibraryCommandsFixture{storage.library(), changes, executor};
    auto snapshot = fixture.library().snapshot();
    auto const targetIds = std::array{trackId};
    auto boundRes = fixture.library().bindTrackTargets(targetIds, snapshot);
    REQUIRE(boundRes);
    auto const revision = snapshot.revision();

    auto const appliedRes =
      fixture.runTask(fixture.commands().updateMetadataAsync(*boundRes, MetadataPatch{.optTitle = "Committed"}));
    REQUIRE(appliedRes);
    CHECK(appliedRes->status == AuthoringStatus::Applied);
    REQUIRE(appliedRes->optNextTargets);
    CHECK(appliedRes->optNextTargets->revision() == revision + 1);
    auto const optOldRow = snapshot.trackRow(trackId);
    REQUIRE(optOldRow);
    CHECK(optOldRow->title == "Before");
    CHECK(snapshot.revision() == revision);
    CHECK(snapshot.containsTrack(trackId));

    auto const rejectedRes = fixture.library().bindTrackTargets(targetIds, snapshot);
    REQUIRE_FALSE(rejectedRes);
    CHECK(rejectedRes.error().code == Error::Code::InvalidState);
    CHECK_FALSE(boundRes->matches(fixture.library().authoringAvailability()));

    auto const staleRes =
      fixture.runTask(fixture.commands().updateMetadataAsync(*boundRes, MetadataPatch{.optTitle = "Must not apply"}));
    REQUIRE(staleRes);
    CHECK(staleRes->status == AuthoringStatus::Stale);
    CHECK(staleRes->reply.changes.empty());
    auto const noOpRes = fixture.runTask(
      fixture.commands().updateMetadataAsync(*appliedRes->optNextTargets, MetadataPatch{.optTitle = "Committed"}));
    REQUIRE(noOpRes);
    CHECK(noOpRes->status == AuthoringStatus::NoOp);
    CHECK(noOpRes->reply.changes.empty());
    CHECK(fixture.library().authoringAvailability().libraryRevision == revision + 1);
    auto const optCurrentRow = fixture.library().snapshot().trackRow(trackId);
    REQUIRE(optCurrentRow);
    CHECK(optCurrentRow->title == "Committed");
  }

  TEST_CASE("Library snapshot binding - closed lifecycle rejects an otherwise current snapshot",
            "[runtime][unit][library-authoring]")
  {
    auto storage = MusicLibraryFixture{};
    auto const trackId = storage.addTrack(library::test::TrackSpec{.title = "Unchanged"});
    auto executor = async::LoopExecutor{};
    auto changes = makeLibraryChanges(executor, storage.library());
    auto fixture = LibraryCommandsFixture{storage.library(), changes, executor};
    auto snapshot = fixture.library().snapshot();
    REQUIRE(fixture.library().bindTrackTargets(std::array{trackId}, snapshot));

    fixture.library().beginClosing();
    auto const boundRes = fixture.library().bindTrackTargets(std::array{trackId}, snapshot);

    REQUIRE_FALSE(boundRes);
    CHECK(boundRes.error().code == Error::Code::InvalidState);
    CHECK(snapshot.containsTrack(trackId));
  }

  TEST_CASE("Library snapshot binding - maintenance rejects a current snapshot until availability returns",
            "[runtime][unit][library-authoring]")
  {
    auto storage = MusicLibraryFixture{};
    auto const trackId = storage.addTrack(library::test::TrackSpec{.title = "Unchanged"});
    auto executor = async::LoopExecutor{};
    auto changes = makeLibraryChanges(executor, storage.library());
    auto fixture = LibraryCommandsFixture{storage.library(), changes, executor};
    auto snapshot = fixture.library().snapshot();
    fixture.releaseLibrary();
    auto laneChanges = makeLibraryChanges(executor, storage.library());
    auto capabilityRes = library::WritableMusicLibrary::acquire(storage.library());
    REQUIRE(capabilityRes);
    auto lane = LibraryWriteLane{executor, std::move(*capabilityRes), laneChanges};
    auto const targetIds = std::array{trackId};
    auto const boundRes = lane.bindTrackTargets(targetIds, snapshot);
    REQUIRE(boundRes);

    auto maintenanceRes = fixture.runTask(LibraryWriteLane::beginMaintenanceAsync(lane.captureSubmission()));
    REQUIRE(maintenanceRes);
    CHECK(lane.availability().state == LibraryAuthoringState::Maintenance);
    auto const rejectedRes = lane.bindTrackTargets(targetIds, snapshot);
    REQUIRE_FALSE(rejectedRes);
    CHECK(rejectedRes.error().code == Error::Code::InvalidState);
    CHECK_FALSE(boundRes->matches(lane.availability()));
    CHECK(snapshot.revision() == lane.availability().libraryRevision);

    fixture.runTask(maintenanceRes->finishAsync());
    auto const availableRes = lane.bindTrackTargets(targetIds, snapshot);
    REQUIRE(availableRes);
    CHECK(availableRes->matches(lane.availability()));
    CHECK(availableRes->revision() == snapshot.revision());
    auto const optRow = snapshot.trackRow(trackId);
    REQUIRE(optRow);
    CHECK(optRow->title == "Unchanged");
  }

  TEST_CASE("Library snapshot binding - same storage snapshot stamps the current runtime instance",
            "[runtime][unit][library-authoring]")
  {
    auto storage = MusicLibraryFixture{};
    auto const trackId = storage.addTrack(library::test::TrackSpec{.title = "Unchanged"});
    auto executor = async::LoopExecutor{};
    auto changes = makeLibraryChanges(executor, storage.library());
    auto fixture = LibraryCommandsFixture{storage.library(), changes, executor};
    auto snapshot = fixture.library().snapshot();
    auto const targetIds = std::array{trackId};
    auto oldRes = fixture.library().bindTrackTargets(targetIds, snapshot);
    REQUIRE(oldRes);
    fixture.releaseLibrary();
    auto replacementChanges = makeLibraryChanges(executor, storage.library());
    auto replacement = LibraryCommandsFixture{storage.library(), replacementChanges, executor};

    auto const currentRes = replacement.library().bindTrackTargets(targetIds, snapshot);

    REQUIRE(currentRes);
    CHECK(currentRes->revision() == snapshot.revision());
    CHECK(currentRes->matches(replacement.library().authoringAvailability()));
    CHECK_FALSE(oldRes->matches(replacement.library().authoringAvailability()));
    auto const staleRes = replacement.runTask(
      replacement.commands().updateMetadataAsync(*oldRes, MetadataPatch{.optTitle = "Must not apply"}));
    REQUIRE(staleRes);
    CHECK(staleRes->status == AuthoringStatus::Stale);
    auto const optRow = replacement.library().snapshot().trackRow(trackId);
    REQUIRE(optRow);
    CHECK(optRow->title == "Unchanged");
  }
} // namespace ao::rt::test
