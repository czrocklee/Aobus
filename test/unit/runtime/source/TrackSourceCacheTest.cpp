// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/source/TrackSourceCache.h>

#include "test/unit/TestFixtureSupport.h"
#include "test/unit/library/WritableLibraryTestSupport.h"
#include "test/unit/runtime/RuntimeLibraryTestSupport.h"
#include "test/unit/runtime/source/TrackSourceTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Subscription.h>
#include <ao/library/ListBuilder.h>
#include <ao/library/ListStore.h>
#include <ao/library/MusicLibrary.h>
#include <ao/rt/TrackEditScript.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/rt/library/LibraryWriter.h>
#include <ao/rt/source/SmartListEvaluator.h>
#include <ao/rt/source/TrackSource.h>
#include <ao/rt/source/TrackSourceDelta.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace ao::rt::test
{
  using namespace ao::library;

  TEST_CASE("TrackSourceCache - source lookup and reload maintain source state",
            "[runtime][unit][source][track-source-cache]")
  {
    auto libraryFixture = MusicLibraryFixture{};

    SECTION("acquire resolves only the explicit All Tracks virtual id")
    {
      auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
      auto cache = TrackSourceCache{libraryFixture.library(), changes};
      auto const allTracksResult = cache.acquire(kAllTracksListId);
      REQUIRE(allTracksResult);
      auto const secondAllTracksResult = cache.acquire(kAllTracksListId);
      REQUIRE(secondAllTracksResult);
      CHECK(&allTracksResult->source() == &secondAllTracksResult->source());

      auto const invalidResult = cache.acquire(kInvalidListId);
      REQUIRE_FALSE(invalidResult);
      CHECK(invalidResult.error().code == Error::Code::InvalidInput);
    }

    SECTION("acquire creates and reuses one saved List identity")
    {
      auto listId = ListId{0};
      {
        auto transaction = library::test::writeTransaction(libraryFixture.library());
        auto builder = ListBuilder::makeEmpty();
        builder.name("Saved List");
        listId = ao::test::requireValue(
          libraryFixture.library().lists().writer(transaction).create(ao::test::requireValue(builder.serialize())));
        REQUIRE(transaction.commit());
      }

      auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
      auto cache = TrackSourceCache{libraryFixture.library(), changes};
      auto firstLease = ao::test::requireValue(cache.acquire(listId));
      auto& source = firstLease.source();
      CHECK(source.state() == TrackSourceState::Live);

      auto secondLease = ao::test::requireValue(cache.acquire(listId));
      CHECK(&source == &secondLease.source());
    }

    SECTION("acquire creates a live smart list identity")
    {
      auto listId = ListId{0};
      {
        auto transaction = library::test::writeTransaction(libraryFixture.library());
        auto builder = ListBuilder::makeEmpty();
        builder.name("SmartList");
        builder.filter("title == \"foo\"");
        listId = ao::test::requireValue(
          libraryFixture.library().lists().writer(transaction).create(ao::test::requireValue(builder.serialize())));
        REQUIRE(transaction.commit());
      }

      auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
      auto cache = TrackSourceCache{libraryFixture.library(), changes};
      auto lease = ao::test::requireValue(cache.acquire(listId));
      CHECK(lease->state() == TrackSourceState::Live);
    }

    SECTION("acquire rejects a missing list without an All Tracks fallback")
    {
      auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
      auto cache = TrackSourceCache{libraryFixture.library(), changes};
      auto const result = cache.acquire(ListId{999});
      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::NotFound);
    }

    SECTION("reloadAllTracks updates allTracks source")
    {
      libraryFixture.addTrack("Track 1");
      libraryFixture.addTrack("Track 2");

      auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
      auto cache = TrackSourceCache{libraryFixture.library(), changes};
      auto allTracks = ao::test::requireValue(cache.acquire(kAllTracksListId));
      cache.reloadAllTracks();
      CHECK(allTracks->size() == 2);
    }

    SECTION("track delete notifications remove allTracks membership")
    {
      auto const trackId = libraryFixture.addTrack("Track 1");
      auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
      auto cache = TrackSourceCache{libraryFixture.library(), changes};
      auto allTracks = ao::test::requireValue(cache.acquire(kAllTracksListId));
      cache.reloadAllTracks();
      REQUIRE(allTracks->size() == 1);
      auto spy = TrackSourceBatchSpy{allTracks.source()};
      auto writerFixture = LibraryWriterFixture{libraryFixture.library(), changes};
      auto& writer = writerFixture.writer();

      CHECK(writer.deleteTrack(trackId).has_value());
      CHECK(allTracks->size() == 0);
      REQUIRE(spy.batches.size() == 1);
      REQUIRE(sourceEditScript(spy.batches.front()).edits.size() == 1);
      auto const& removal = std::get<delta::RemoveRange>(sourceEditScript(spy.batches.front()).edits.front());
      CHECK(removal.start == 0);
      CHECK(removal.trackIds == std::vector{trackId});
    }

    SECTION("LibraryWriter integration")
    {
      auto listId = ListId{0};
      {
        auto transaction = library::test::writeTransaction(libraryFixture.library());
        auto builder = ListBuilder::makeEmpty();
        builder.name("ToErase");
        listId = ao::test::requireValue(
          libraryFixture.library().lists().writer(transaction).create(ao::test::requireValue(builder.serialize())));
        REQUIRE(transaction.commit());
      }

      auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
      auto cache = TrackSourceCache{libraryFixture.library(), changes};
      auto lease = ao::test::requireValue(cache.acquire(listId));
      auto writerFixture = LibraryWriterFixture{libraryFixture.library(), changes};
      auto& writer = writerFixture.writer();

      REQUIRE(writer.deleteList(listId));

      CHECK(lease->state() == TrackSourceState::Invalidated);
      auto const result = cache.acquire(listId);
      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::NotFound);
    }
  }

  TEST_CASE("TrackSourceCache - headless metadata mutation updates all-tracks and smart membership once",
            "[runtime][workflow][source][track-source-cache]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const trackId = libraryFixture.addTrack("Before");
    auto smartListId = kInvalidListId;

    {
      auto transaction = library::test::writeTransaction(libraryFixture.library());
      auto builder = ListBuilder::makeEmpty();
      builder.name("Matching title");
      builder.filter("$title = \"After\"");
      smartListId = ao::test::requireValue(
        libraryFixture.library().lists().writer(transaction).create(ao::test::requireValue(builder.serialize())));
      REQUIRE(transaction.commit());
    }

    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto writerFixture = LibraryWriterFixture{libraryFixture.library(), changes};
    auto cache = TrackSourceCache{libraryFixture.library(), changes};
    cache.reloadAllTracks();
    auto allTracksLease = ao::test::requireValue(cache.acquire(kAllTracksListId));
    auto smartLease = ao::test::requireValue(cache.acquire(smartListId));
    auto& smartSource = smartLease.source();
    REQUIRE(smartSource.size() == 0);

    auto allTracksBatches = std::vector<TrackSourceDelta>{};
    auto smartBatches = std::vector<TrackSourceDelta>{};
    auto allTracksSubscription =
      allTracksLease->subscribe([&](TrackSourceDelta const& batch) noexcept { allTracksBatches.push_back(batch); });
    auto smartSubscription =
      smartSource.subscribe([&](TrackSourceDelta const& batch) noexcept { smartBatches.push_back(batch); });

    auto const result = writerFixture.updateMetadata(std::array{trackId}, MetadataPatch{.optTitle = "After"});

    REQUIRE(result);
    REQUIRE(smartSource.size() == 1);
    CHECK(smartSource.trackIdAt(0) == trackId);
    REQUIRE(allTracksBatches.size() == 1);
    REQUIRE(sourceEditScript(allTracksBatches[0]).edits.size() == 1);
    auto const& update = std::get<delta::UpdateRange>(sourceEditScript(allTracksBatches[0]).edits[0]);
    CHECK(update.start == 0);
    CHECK(update.trackIds == std::vector{trackId});

    REQUIRE(smartBatches.size() == 1);
    REQUIRE(sourceEditScript(smartBatches[0]).edits.size() == 1);
    auto const& insert = std::get<delta::InsertRange>(sourceEditScript(smartBatches[0]).edits[0]);
    CHECK(insert.start == 0);
    CHECK(insert.trackIds == std::vector{trackId});
  }

  TEST_CASE("TrackSourceCache - shutdown does not semantically invalidate a leased All Tracks source",
            "[runtime][unit][source][track-source-cache]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const trackId = libraryFixture.addTrack("Pinned across cache shutdown");
    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto optLease = std::optional<TrackSourceLease>{};
    auto batches = std::vector<TrackSourceDelta>{};
    auto subscription = async::Subscription{};

    {
      auto cache = TrackSourceCache{libraryFixture.library(), changes};
      cache.reloadAllTracks();
      optLease.emplace(ao::test::requireValue(cache.acquire(kAllTracksListId)));
      subscription =
        (*optLease)->subscribe([&batches](TrackSourceDelta const& batch) noexcept { batches.push_back(batch); });

      REQUIRE((*optLease)->size() == 1);
      CHECK((*optLease)->trackIdAt(0) == trackId);
      CHECK((*optLease)->state() == TrackSourceState::Live);
    }

    REQUIRE(optLease);
    CHECK((*optLease)->state() == TrackSourceState::Live);
    REQUIRE((*optLease)->size() == 1);
    CHECK((*optLease)->trackIdAt(0) == trackId);
    CHECK(batches.empty());
    subscription.reset();
  }

  TEST_CASE("TrackSourceCache - cached sources preserve mutation delivery, identity, and deletion invalidation",
            "[runtime][unit][source][track-source-cache]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const trackId = libraryFixture.addTrack("Inserted after acquisition");
    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto writerFixture = LibraryWriterFixture{libraryFixture.library(), changes};
    auto& writer = writerFixture.writer();
    auto const listId = ao::test::requireValue(writer.createList(LibraryWriter::ListDraft{
      .name = "Leased",
      .expression = "false",
    }));
    auto cache = TrackSourceCache{libraryFixture.library(), changes};
    cache.reloadAllTracks();
    auto lease = ao::test::requireValue(cache.acquire(listId));
    auto* const identity = &lease.source();

    auto reacquired = ao::test::requireValue(cache.acquire(listId));
    CHECK(&reacquired.source() == identity);

    auto batches = std::vector<TrackSourceDelta>{};
    auto subscription = lease->subscribe([&](TrackSourceDelta const& batch) noexcept { batches.push_back(batch); });

    REQUIRE(writer.updateList(LibraryWriter::ListDraft{
      .listId = listId,
      .name = "Leased",
      .expression = "true",
    }));

    REQUIRE(lease->size() == 1);
    CHECK(lease->trackIdAt(0) == trackId);
    REQUIRE(batches.size() == 1);
    CHECK(std::holds_alternative<SourceReset>(batches[0]));
    batches.clear();

    REQUIRE(writer.deleteList(listId));

    CHECK(lease->state() == TrackSourceState::Invalidated);
    REQUIRE(batches.size() == 1);
    CHECK(std::holds_alternative<SourceInvalidated>(batches[0]));
    auto const missingResult = cache.acquire(listId);
    REQUIRE_FALSE(missingResult);
    CHECK(missingResult.error().code == Error::Code::NotFound);
  }

  TEST_CASE("TrackSourceCache - same-id recreation creates a new identity beside the invalidated lease",
            "[runtime][unit][source][track-source-cache]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto writerFixture = LibraryWriterFixture{libraryFixture.library(), changes};
    auto& writer = writerFixture.writer();
    auto const listId = ao::test::requireValue(writer.createList(LibraryWriter::ListDraft{
      .name = "Original",
    }));
    auto cache = TrackSourceCache{libraryFixture.library(), changes};
    auto oldLease = ao::test::requireValue(cache.acquire(listId));
    auto* const oldIdentity = &oldLease.source();
    auto oldBatches = std::vector<TrackSourceDelta>{};
    auto oldSubscription =
      oldLease->subscribe([&](TrackSourceDelta const& batch) noexcept { oldBatches.push_back(batch); });

    REQUIRE(writer.deleteList(listId));
    REQUIRE(oldLease->state() == TrackSourceState::Invalidated);
    REQUIRE(oldBatches.size() == 1);
    CHECK(std::holds_alternative<SourceInvalidated>(oldBatches[0]));

    auto const recreatedId = ao::test::requireValue(writer.createList(LibraryWriter::ListDraft{
      .name = "Recreated",
    }));
    REQUIRE(recreatedId == listId);
    auto newLease = ao::test::requireValue(cache.acquire(recreatedId));

    CHECK(&newLease.source() != oldIdentity);
    CHECK(newLease->state() == TrackSourceState::Live);
    CHECK(oldLease->state() == TrackSourceState::Invalidated);
    CHECK(oldBatches.size() == 1);
  }

  TEST_CASE("TrackSourceCache - rejected parent deletion leaves its cached dependency graph live",
            "[runtime][unit][source][track-source-cache]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto writerFixture = LibraryWriterFixture{libraryFixture.library(), changes};
    auto& writer = writerFixture.writer();
    auto const parentId = ao::test::requireValue(writer.createList(LibraryWriter::ListDraft{
      .name = "Parent",
    }));
    auto const childId = ao::test::requireValue(writer.createList(LibraryWriter::ListDraft{
      .parentId = parentId,
      .name = "Child",
    }));
    auto cache = TrackSourceCache{libraryFixture.library(), changes};
    auto parentLease = ao::test::requireValue(cache.acquire(parentId));
    auto childLease = ao::test::requireValue(cache.acquire(childId));
    auto parentAgain = ao::test::requireValue(cache.acquire(parentId));
    CHECK(&parentAgain.source() == &parentLease.source());

    auto parentBatches = std::vector<TrackSourceDelta>{};
    auto childBatches = std::vector<TrackSourceDelta>{};
    auto parentSubscription =
      parentAgain->subscribe([&](TrackSourceDelta const& batch) noexcept { parentBatches.push_back(batch); });
    auto childSubscription =
      childLease->subscribe([&](TrackSourceDelta const& batch) noexcept { childBatches.push_back(batch); });

    auto const rejected = writer.deleteList(parentId);
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().code == Error::Code::Conflict);

    CHECK(parentAgain->state() == TrackSourceState::Live);
    CHECK(childLease->state() == TrackSourceState::Live);
    CHECK(parentBatches.empty());
    CHECK(childBatches.empty());

    REQUIRE(writer.deleteList(childId));
    CHECK(parentAgain->state() == TrackSourceState::Live);
    CHECK(childLease->state() == TrackSourceState::Invalidated);
    CHECK(parentBatches.empty());
    REQUIRE(childBatches.size() == 1);
    CHECK(std::holds_alternative<SourceInvalidated>(childBatches[0]));

    REQUIRE(writer.deleteList(parentId));
    CHECK(parentAgain->state() == TrackSourceState::Invalidated);
    REQUIRE(parentBatches.size() == 1);
    CHECK(std::holds_alternative<SourceInvalidated>(parentBatches[0]));
  }

  TEST_CASE("TrackSourceCache - reparent rewires deletion propagation to the new parent",
            "[runtime][unit][source][track-source-cache]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto writerFixture = LibraryWriterFixture{libraryFixture.library(), changes};
    auto& writer = writerFixture.writer();
    auto const oldParentId = ao::test::requireValue(writer.createList(LibraryWriter::ListDraft{
      .name = "Old parent",
    }));
    auto const newParentId = ao::test::requireValue(writer.createList(LibraryWriter::ListDraft{
      .name = "New parent",
    }));
    auto const childId = ao::test::requireValue(writer.createList(LibraryWriter::ListDraft{
      .parentId = oldParentId,
      .name = "Child",
    }));
    auto cache = TrackSourceCache{libraryFixture.library(), changes};
    auto childLease = ao::test::requireValue(cache.acquire(childId));
    auto* const childIdentity = &childLease.source();
    auto childBatches = std::vector<TrackSourceDelta>{};
    auto childSubscription =
      childLease->subscribe([&](TrackSourceDelta const& batch) noexcept { childBatches.push_back(batch); });

    REQUIRE(writer.updateList(LibraryWriter::ListDraft{
      .parentId = newParentId,
      .listId = childId,
      .name = "Child",
    }));

    CHECK(&childLease.source() == childIdentity);
    CHECK(childLease->state() == TrackSourceState::Live);
    REQUIRE(childBatches.size() == 1);
    CHECK(std::holds_alternative<SourceReset>(childBatches[0]));

    REQUIRE(writer.deleteList(oldParentId));
    CHECK(childLease->state() == TrackSourceState::Live);
    CHECK(childBatches.size() == 1);

    auto const rejected = writer.deleteList(newParentId);
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().code == Error::Code::Conflict);
    CHECK(childLease->state() == TrackSourceState::Live);
    CHECK(childBatches.size() == 1);

    REQUIRE(writer.deleteList(childId));
    CHECK(childLease->state() == TrackSourceState::Invalidated);
    REQUIRE(childBatches.size() == 2);
    CHECK(std::holds_alternative<SourceInvalidated>(childBatches[1]));
    REQUIRE(writer.deleteList(newParentId));
  }

  TEST_CASE("TrackSourceCache - definition rebind keeps identity and metadata-only updates emit nothing",
            "[runtime][unit][source][track-source-cache]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto writerFixture = LibraryWriterFixture{libraryFixture.library(), changes};
    auto& writer = writerFixture.writer();
    auto draft = LibraryWriter::ListDraft{
      .name = "Before",
    };
    auto const listId = ao::test::requireValue(writer.createList(draft));
    auto cache = TrackSourceCache{libraryFixture.library(), changes};
    auto lease = ao::test::requireValue(cache.acquire(listId));
    auto* const identity = &lease.source();
    auto batches = std::vector<TrackSourceDelta>{};
    auto subscription = lease->subscribe([&](TrackSourceDelta const& batch) noexcept { batches.push_back(batch); });

    draft.listId = listId;
    draft.name = "Metadata only";
    REQUIRE(writer.updateList(draft));
    CHECK(batches.empty());

    draft.expression = "true";
    REQUIRE(writer.updateList(draft));

    CHECK(&lease.source() == identity);
    CHECK(lease->state() == TrackSourceState::Live);
    REQUIRE(batches.size() == 1);
    CHECK(std::holds_alternative<SourceReset>(batches[0]));
  }

  TEST_CASE("TrackSourceCache - detailed List order moves publish exact batches and reset explicitly",
            "[runtime][unit][source][track-source-cache]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const first = libraryFixture.addTrack("First");
    auto const second = libraryFixture.addTrack("Second");
    auto const third = libraryFixture.addTrack("Third");
    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto writerFixture = LibraryWriterFixture{libraryFixture.library(), changes};
    auto& writer = writerFixture.writer();
    auto const listId = ao::test::requireValue(writer.createList(LibraryWriter::ListDraft{
      .name = "Detailed",
    }));
    auto cache = TrackSourceCache{libraryFixture.library(), changes};
    cache.reloadAllTracks();
    auto lease = ao::test::requireValue(cache.acquire(listId));
    auto batches = std::vector<TrackSourceDelta>{};
    [[maybe_unused]] auto subscription =
      lease->subscribe([&batches](TrackSourceDelta const& batch) noexcept { batches.push_back(batch); });

    auto effectiveTrackIds = sourceTrackIds(lease.source());
    auto binding = ao::test::requireValue(writerFixture.library().bindListOrder(listId, effectiveTrackIds));
    auto const moveResult = writer.moveListOrder(binding, std::array{second}, first);
    REQUIRE(moveResult);
    REQUIRE(moveResult->status == ListOrderAuthoringStatus::Applied);
    REQUIRE(batches.size() == 1);
    REQUIRE(sourceEditScript(batches[0]).edits.size() == 2);
    CHECK(std::holds_alternative<delta::RemoveRange>(sourceEditScript(batches[0]).edits[0]));
    CHECK(std::holds_alternative<delta::InsertRange>(sourceEditScript(batches[0]).edits[1]));
    CHECK(sourceTrackIds(lease.source()) == std::vector{second, first, third});

    effectiveTrackIds = sourceTrackIds(lease.source());
    binding = ao::test::requireValue(writerFixture.library().bindListOrder(listId, effectiveTrackIds));
    auto const resetResult = writer.resetListOrder(binding);
    REQUIRE(resetResult);
    REQUIRE(resetResult->status == ListOrderAuthoringStatus::Applied);
    REQUIRE(batches.size() == 2);
    CHECK(std::holds_alternative<SourceReset>(batches[1]));
    CHECK(sourceTrackIds(lease.source()) == std::vector{first, second, third});
  }

  TEST_CASE("TrackSourceCache - reentrant metadata mutation is rejected during detailed publication",
            "[runtime][regression][source][list-order]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const first = libraryFixture.addTrack("First");
    auto const inserted = libraryFixture.addTrack("Inserted");
    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto writerFixture = LibraryWriterFixture{libraryFixture.library(), changes};
    auto& writer = writerFixture.writer();
    auto const listId = ao::test::requireValue(writer.createList(LibraryWriter::ListDraft{
      .name = "Before",
    }));
    auto cache = TrackSourceCache{libraryFixture.library(), changes};
    cache.reloadAllTracks();
    auto lease = ao::test::requireValue(cache.acquire(listId));
    auto* const identity = &lease.source();
    auto batches = std::vector<TrackSourceDelta>{};
    bool handledMove = false;
    auto nestedError = Error::Code::Generic;
    [[maybe_unused]] auto subscription = lease->subscribe(
      [&](TrackSourceDelta const& batch) noexcept
      {
        batches.push_back(batch);

        if (handledMove)
        {
          return;
        }

        handledMove = true;
        auto const result = writer.updateList(LibraryWriter::ListDraft{
          .listId = listId,
          .name = "Renamed while publishing",
        });

        if (!result)
        {
          nestedError = result.error().code;
        }
      });

    auto const effectiveTrackIds = sourceTrackIds(lease.source());
    auto binding = ao::test::requireValue(writerFixture.library().bindListOrder(listId, effectiveTrackIds));
    auto const result = writer.moveListOrder(binding, std::array{inserted}, first);

    REQUIRE(result);
    REQUIRE(result->status == ListOrderAuthoringStatus::Applied);
    CHECK(nestedError == Error::Code::InvalidState);
    CHECK(&lease.source() == identity);
    CHECK(lease->state() == TrackSourceState::Live);
    CHECK(sourceTrackIds(lease.source()) == std::vector{inserted, first});
    REQUIRE(batches.size() == 1);
    REQUIRE(sourceEditScript(batches.front()).edits.size() == 2);
    CHECK(std::holds_alternative<delta::RemoveRange>(sourceEditScript(batches.front()).edits[0]));
    CHECK(std::holds_alternative<delta::InsertRange>(sourceEditScript(batches.front()).edits[1]));

    auto const transaction = libraryFixture.library().readTransaction();
    auto const optView = libraryFixture.library().lists().reader(transaction).get(listId);
    REQUIRE(optView);
    CHECK(optView->name() == "Before");
  }

  TEST_CASE("TrackSourceCache - reentrant reparent is rejected during detailed publication",
            "[runtime][regression][source][list-order]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const first = libraryFixture.addTrack("First");
    auto const inserted = libraryFixture.addTrack("Inserted");
    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto writerFixture = LibraryWriterFixture{libraryFixture.library(), changes};
    auto& writer = writerFixture.writer();
    auto const oldParentId = ao::test::requireValue(writer.createList(LibraryWriter::ListDraft{
      .name = "Old parent",
    }));
    auto const newParentId = ao::test::requireValue(writer.createList(LibraryWriter::ListDraft{
      .name = "New parent",
    }));
    auto const childId = ao::test::requireValue(writer.createList(LibraryWriter::ListDraft{
      .parentId = oldParentId,
      .name = "Child",
    }));
    auto cache = TrackSourceCache{libraryFixture.library(), changes};
    cache.reloadAllTracks();
    auto childLease = ao::test::requireValue(cache.acquire(childId));
    auto* const identity = &childLease.source();
    auto batches = std::vector<TrackSourceDelta>{};
    auto trailingBatches = std::vector<TrackSourceDelta>{};
    auto snapshotAfterNestedUpdate = std::vector<TrackId>{};
    bool handledMove = false;
    auto nestedError = Error::Code::Generic;
    [[maybe_unused]] auto subscription = childLease->subscribe(
      [&](TrackSourceDelta const& batch) noexcept
      {
        batches.push_back(batch);

        if (handledMove)
        {
          return;
        }

        handledMove = true;
        auto const result = writer.updateList(LibraryWriter::ListDraft{
          .parentId = newParentId,
          .listId = childId,
          .name = "Child",
        });

        if (!result)
        {
          nestedError = result.error().code;
        }

        snapshotAfterNestedUpdate = sourceTrackIds(childLease.source());
      });
    [[maybe_unused]] auto trailingSubscription =
      childLease->subscribe([&](TrackSourceDelta const& batch) noexcept { trailingBatches.push_back(batch); });

    auto const effectiveTrackIds = sourceTrackIds(childLease.source());
    auto binding = ao::test::requireValue(writerFixture.library().bindListOrder(childId, effectiveTrackIds));
    auto const result = writer.moveListOrder(binding, std::array{inserted}, first);

    REQUIRE(result);
    REQUIRE(result->status == ListOrderAuthoringStatus::Applied);
    CHECK(nestedError == Error::Code::InvalidState);
    CHECK(snapshotAfterNestedUpdate == std::vector{inserted, first});
    CHECK(&childLease.source() == identity);
    CHECK(childLease->state() == TrackSourceState::Live);
    CHECK(sourceTrackIds(childLease.source()) == std::vector{inserted, first});
    REQUIRE(batches.size() == 1);
    REQUIRE(sourceEditScript(batches[0]).edits.size() == 2);
    CHECK(std::holds_alternative<delta::RemoveRange>(sourceEditScript(batches[0]).edits[0]));
    CHECK(std::holds_alternative<delta::InsertRange>(sourceEditScript(batches[0]).edits[1]));
    REQUIRE(trailingBatches.size() == 1);
    CHECK(std::holds_alternative<delta::RemoveRange>(sourceEditScript(trailingBatches[0]).edits.front()));

    {
      auto const transaction = libraryFixture.library().readTransaction();
      auto const optView = libraryFixture.library().lists().reader(transaction).get(childId);
      REQUIRE(optView);
      CHECK(optView->parentId() == oldParentId);
    }

    REQUIRE(writer.deleteList(newParentId));
    CHECK(childLease->state() == TrackSourceState::Live);
    CHECK(batches.size() == 1);

    auto const parentDelete = writer.deleteList(oldParentId);
    REQUIRE_FALSE(parentDelete);
    CHECK(parentDelete.error().code == Error::Code::Conflict);

    REQUIRE(writer.deleteList(childId));
    CHECK(childLease->state() == TrackSourceState::Invalidated);
    REQUIRE(batches.size() == 2);
    CHECK(std::holds_alternative<SourceInvalidated>(batches[1]));
    REQUIRE(writer.deleteList(oldParentId));
  }

  TEST_CASE("TrackSourceCache - mutations reentered from a delta observer are rejected",
            "[runtime][regression][source][list-order]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const first = libraryFixture.addTrack("First");
    auto const second = libraryFixture.addTrack("Second");
    auto const third = libraryFixture.addTrack("Third");
    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto writerFixture = LibraryWriterFixture{libraryFixture.library(), changes};
    auto& writer = writerFixture.writer();
    auto const oldParentId = ao::test::requireValue(writer.createList(LibraryWriter::ListDraft{
      .name = "Old parent",
    }));
    auto const intermediateParentId = ao::test::requireValue(writer.createList(LibraryWriter::ListDraft{
      .name = "Intermediate parent",
    }));
    auto const finalParentId = ao::test::requireValue(writer.createList(LibraryWriter::ListDraft{
      .name = "Final parent",
    }));
    auto const childId = ao::test::requireValue(writer.createList(LibraryWriter::ListDraft{
      .parentId = oldParentId,
      .name = "Child",
    }));
    auto cache = TrackSourceCache{libraryFixture.library(), changes};
    cache.reloadAllTracks();
    auto childLease = ao::test::requireValue(cache.acquire(childId));
    auto* const identity = &childLease.source();
    auto batches = std::vector<TrackSourceDelta>{};
    bool callbackInvoked = false;
    auto nestedMoveStatus = ListOrderAuthoringStatus::NoOp;
    auto intermediateReparentError = Error::Code::Generic;
    auto finalReparentError = Error::Code::Generic;
    auto const initialTrackIds = sourceTrackIds(childLease.source());
    auto outerBinding = ao::test::requireValue(writerFixture.library().bindListOrder(childId, initialTrackIds));
    auto nestedBinding = outerBinding;
    [[maybe_unused]] auto subscription = childLease->subscribe(
      [&](TrackSourceDelta const& batch) noexcept
      {
        batches.push_back(batch);

        if (callbackInvoked)
        {
          return;
        }

        callbackInvoked = true;
        auto const nestedMoveResult = writer.moveListOrder(nestedBinding, std::array{third}, first);

        if (nestedMoveResult)
        {
          nestedMoveStatus = nestedMoveResult->status;
        }
        else
        {
          nestedMoveStatus = ListOrderAuthoringStatus::Unavailable;
        }

        auto const intermediateResult = writer.updateList(LibraryWriter::ListDraft{
          .parentId = intermediateParentId,
          .listId = childId,
          .name = "Child",
        });

        if (!intermediateResult)
        {
          intermediateReparentError = intermediateResult.error().code;
        }

        auto const finalResult = writer.updateList(LibraryWriter::ListDraft{
          .parentId = finalParentId,
          .listId = childId,
          .name = "Child",
        });

        if (!finalResult)
        {
          finalReparentError = finalResult.error().code;
        }
      });

    auto const outerMoveResult = writer.moveListOrder(outerBinding, std::array{second}, first);
    REQUIRE(outerMoveResult);
    REQUIRE(outerMoveResult->status == ListOrderAuthoringStatus::Applied);

    CHECK(nestedMoveStatus == ListOrderAuthoringStatus::Unavailable);
    CHECK(intermediateReparentError == Error::Code::InvalidState);
    CHECK(finalReparentError == Error::Code::InvalidState);
    CHECK(&childLease.source() == identity);
    CHECK(childLease->state() == TrackSourceState::Live);
    CHECK(sourceTrackIds(childLease.source()) == std::vector{second, first, third});
    REQUIRE(batches.size() == 1);
    REQUIRE(sourceEditScript(batches[0]).edits.size() == 2);
    CHECK(std::holds_alternative<delta::RemoveRange>(sourceEditScript(batches[0]).edits[0]));
    CHECK(std::holds_alternative<delta::InsertRange>(sourceEditScript(batches[0]).edits[1]));

    {
      auto const transaction = libraryFixture.library().readTransaction();
      auto const optView = libraryFixture.library().lists().reader(transaction).get(childId);
      REQUIRE(optView);
      CHECK(optView->parentId() == oldParentId);
    }

    // Rejecting the reentrant attempts is not a fault: authoring stays open.
    CHECK(writer.deleteList(finalParentId));
  }

  TEST_CASE("TrackSourceCache - hidden rank re-enters at its stored position",
            "[runtime][unit][source][track-source-cache]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const visible = libraryFixture.addTrack("Visible");
    auto const hidden = libraryFixture.addTrack("Parent hidden");
    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto writerFixture = LibraryWriterFixture{libraryFixture.library(), changes};
    auto& writer = writerFixture.writer();
    auto const membershipTag = std::array{std::string{"parentmember"}};
    REQUIRE(writerFixture.editTags(std::array{visible, hidden}, membershipTag, {}));
    auto const parentId = ao::test::requireValue(writer.createList(LibraryWriter::ListDraft{
      .name = "Parent",
      .expression = "#parentmember",
    }));
    auto const childId = ao::test::requireValue(writer.createList(LibraryWriter::ListDraft{
      .parentId = parentId,
      .name = "Child",
    }));
    auto cache = TrackSourceCache{libraryFixture.library(), changes};
    cache.reloadAllTracks();
    auto childLease = ao::test::requireValue(cache.acquire(childId));
    auto batches = std::vector<TrackSourceDelta>{};
    [[maybe_unused]] auto subscription =
      childLease->subscribe([&batches](TrackSourceDelta const& batch) noexcept { batches.push_back(batch); });

    auto effectiveTrackIds = sourceTrackIds(childLease.source());
    auto binding = ao::test::requireValue(writerFixture.library().bindListOrder(childId, effectiveTrackIds));
    auto firstMove = writer.moveListOrder(binding, std::array{hidden}, visible);
    REQUIRE(firstMove);
    REQUIRE(firstMove->status == ListOrderAuthoringStatus::Applied);

    effectiveTrackIds = sourceTrackIds(childLease.source());
    binding = ao::test::requireValue(writerFixture.library().bindListOrder(childId, effectiveTrackIds));
    auto secondMove = writer.moveListOrder(binding, std::array{visible}, hidden);
    REQUIRE(secondMove);
    REQUIRE(secondMove->status == ListOrderAuthoringStatus::Applied);
    CHECK(sourceTrackIds(childLease.source()) == std::vector{visible, hidden});

    REQUIRE(writerFixture.editTags(std::array{hidden}, {}, membershipTag));
    CHECK(sourceTrackIds(childLease.source()) == std::vector{visible});
    batches.clear();

    REQUIRE(writerFixture.editTags(std::array{hidden}, membershipTag, {}));

    auto const expected = std::vector{visible, hidden};
    REQUIRE(batches.size() == 1);
    REQUIRE(sourceEditScript(batches.front()).edits.size() == 1);
    auto const& insertion = std::get<delta::InsertRange>(sourceEditScript(batches.front()).edits.front());
    CHECK(insertion.start == 1);
    CHECK(insertion.trackIds == std::vector{hidden});
    CHECK(sourceTrackIds(childLease.source()) == expected);
  }

  TEST_CASE("TrackSourceCache - track deletion does not duplicate order removal via parent",
            "[runtime][unit][source][track-source-cache]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const first = libraryFixture.addTrack("First");
    auto const deleted = libraryFixture.addTrack("Deleted");
    auto const third = libraryFixture.addTrack("Third");
    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto writerFixture = LibraryWriterFixture{libraryFixture.library(), changes};
    auto& writer = writerFixture.writer();
    auto const listId = ao::test::requireValue(writer.createList(LibraryWriter::ListDraft{
      .name = "Delete target",
    }));
    auto cache = TrackSourceCache{libraryFixture.library(), changes};
    cache.reloadAllTracks();
    auto lease = ao::test::requireValue(cache.acquire(listId));
    auto batches = std::vector<TrackSourceDelta>{};
    [[maybe_unused]] auto subscription =
      lease->subscribe([&batches](TrackSourceDelta const& batch) noexcept { batches.push_back(batch); });

    auto effectiveTrackIds = sourceTrackIds(lease.source());
    auto binding = ao::test::requireValue(writerFixture.library().bindListOrder(listId, effectiveTrackIds));
    auto moveResult = writer.moveListOrder(binding, std::array{deleted}, first);
    REQUIRE(moveResult);
    REQUIRE(moveResult->status == ListOrderAuthoringStatus::Applied);
    batches.clear();

    auto const result = writer.deleteTrack(deleted);

    REQUIRE(result);
    auto const expected = std::vector{first, third};
    REQUIRE(batches.size() == 1);
    REQUIRE(sourceEditScript(batches.front()).edits.size() == 1);
    auto const& removal = std::get<delta::RemoveRange>(sourceEditScript(batches.front()).edits.front());
    CHECK(removal.start == 0);
    CHECK(removal.trackIds == std::vector{deleted});
    CHECK(sourceTrackIds(lease.source()) == expected);
  }

  TEST_CASE("TrackSourceCache - explicit List removal publishes one final visible removal",
            "[runtime][regression][track-source][list-order]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const first = libraryFixture.addTrack("First");
    auto const removed = libraryFixture.addTrack("Removed");
    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto writerFixture = LibraryWriterFixture{libraryFixture.library(), changes};
    auto& writer = writerFixture.writer();
    auto const membershipTag = std::array{std::string{"roadtrip"}};
    REQUIRE(writerFixture.editTags(std::array{first, removed}, membershipTag, {}));
    auto const listId = ao::test::requireValue(writer.createList(LibraryWriter::ListDraft{
      .name = "Road Trip",
      .expression = "#roadtrip",
    }));
    auto cache = TrackSourceCache{libraryFixture.library(), changes};
    cache.reloadAllTracks();
    auto lease = ao::test::requireValue(cache.acquire(listId));
    auto batches = std::vector<TrackSourceDelta>{};
    [[maybe_unused]] auto subscription =
      lease->subscribe([&batches](TrackSourceDelta const& batch) noexcept { batches.push_back(batch); });

    auto effectiveTrackIds = sourceTrackIds(lease.source());
    auto orderBinding = ao::test::requireValue(writerFixture.library().bindListOrder(listId, effectiveTrackIds));
    auto const moveResult = writer.moveListOrder(orderBinding, std::array{removed}, first);
    REQUIRE(moveResult);
    REQUIRE(moveResult->status == ListOrderAuthoringStatus::Applied);
    CHECK(sourceTrackIds(lease.source()) == std::vector{removed, first});
    batches.clear();

    auto const targets = ao::test::requireValue(writerFixture.library().bindTrackTargets(std::array{removed}));
    auto const result = writer.removeTracksFromList(listId, targets);

    REQUIRE(result);
    REQUIRE(result->status == TrackAuthoringStatus::Applied);
    REQUIRE(result->reply.forgottenPositionTrackIds == std::vector{removed});
    REQUIRE(batches.size() == 1);
    auto const& script = sourceEditScript(batches.front());
    REQUIRE(script.edits.size() == 1);
    auto const& removal = std::get<delta::RemoveRange>(script.edits.front());
    CHECK(removal.start == 0);
    CHECK(removal.trackIds == std::vector{removed});
    CHECK(sourceTrackIds(lease.source()) == std::vector{first});
  }

  TEST_CASE("TrackSourceCache - identical source specs share one ad-hoc source", "[runtime][unit][source][source-spec]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    libraryFixture.addTrack("First");
    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto cache = TrackSourceCache{libraryFixture.library(), changes};
    cache.reloadAllTracks();
    auto const spec = SourceSpec{.baseListId = kAllTracksListId, .filterExpression = "true"};

    auto first = ao::test::requireValue(cache.acquire(spec));
    auto second = ao::test::requireValue(cache.acquire(spec));
    auto different =
      ao::test::requireValue(cache.acquire(SourceSpec{.baseListId = kAllTracksListId, .filterExpression = "false"}));

    CHECK(&first.source() == &second.source());
    CHECK(&first.source() != &different.source());
  }

  TEST_CASE("TrackSourceCache - a list stored at the root derives from All Tracks",
            "[runtime][unit][source][track-source-cache]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const kept = libraryFixture.addTrack("Keep");
    libraryFixture.addTrack("Drop");
    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto writerFixture = LibraryWriterFixture{libraryFixture.library(), changes};
    auto const listId = ao::test::requireValue(writerFixture.writer().createList(LibraryWriter::ListDraft{
      .parentId = kInvalidListId,
      .name = "Root smart list",
      .expression = "$title = \"Keep\"",
    }));

    auto cache = TrackSourceCache{libraryFixture.library(), changes};
    cache.reloadAllTracks();

    // A root list stores parentId kInvalidListId, which acquire() rejects on its
    // own. The cache must resolve it to the All Tracks root before recursing.
    auto lease = ao::test::requireValue(cache.acquire(listId));
    CHECK(lease->state() == TrackSourceState::Live);
    CHECK(sourceTrackIds(lease.source()) == std::vector{kept});
  }
} // namespace ao::rt::test
