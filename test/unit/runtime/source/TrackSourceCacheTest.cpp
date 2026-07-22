// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/RuntimeTestSupport.h"
#include "test/unit/TestUtils.h"
#include "test/unit/library/WritableLibraryTestSupport.h"
#include "test/unit/runtime/source/TrackSourceTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/Exception.h>
#include <ao/async/Subscription.h>
#include <ao/library/ListBuilder.h>
#include <ao/library/ListStore.h>
#include <ao/library/MusicLibrary.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryWriter.h>
#include <ao/rt/source/SmartListEvaluator.h>
#include <ao/rt/source/TrackSource.h>
#include <ao/rt/source/TrackSourceCache.h>
#include <ao/rt/source/TrackSourceDelta.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include <array>
#include <optional>
#include <ranges>
#include <variant>
#include <vector>

namespace ao::rt::test
{
  using namespace ao::library;

  TEST_CASE("TrackSourceCache - source lookup and reload maintain source state",
            "[runtime][unit][source][track-source-cache]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto changes = LibraryChanges{};

    auto cache = TrackSourceCache{libraryFixture.library(), changes};

    SECTION("acquire resolves only the explicit All Tracks virtual id")
    {
      auto const allTracksResult = cache.acquire(kAllTracksListId);
      REQUIRE(allTracksResult);
      auto const secondAllTracksResult = cache.acquire(kAllTracksListId);
      REQUIRE(secondAllTracksResult);
      CHECK(&allTracksResult->source() == &secondAllTracksResult->source());

      auto const invalidResult = cache.acquire(kInvalidListId);
      REQUIRE_FALSE(invalidResult);
      CHECK(invalidResult.error().code == Error::Code::InvalidInput);
    }

    SECTION("acquire creates and reuses one manual list identity")
    {
      auto listId = ListId{0};
      {
        auto transaction = library::test::writeTransaction(libraryFixture.library());
        auto builder = ListBuilder::makeEmpty();
        builder.name("ManualList");
        listId =
          ao::test::requireValue(
            libraryFixture.library().lists().writer(transaction).create(ao::test::requireValue(builder.serialize())))
            .first;
        REQUIRE(transaction.commit());
      }

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
        listId =
          ao::test::requireValue(
            libraryFixture.library().lists().writer(transaction).create(ao::test::requireValue(builder.serialize())))
            .first;
        REQUIRE(transaction.commit());
      }

      auto lease = ao::test::requireValue(cache.acquire(listId));
      CHECK(lease->state() == TrackSourceState::Live);
    }

    SECTION("acquire rejects a missing list without an All Tracks fallback")
    {
      auto const result = cache.acquire(ListId{999});
      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::NotFound);
    }

    SECTION("reloadAllTracks updates allTracks source")
    {
      auto allTracks = ao::test::requireValue(cache.acquire(kAllTracksListId));
      libraryFixture.addTrack("Track 1");
      libraryFixture.addTrack("Track 2");

      cache.reloadAllTracks();
      CHECK(allTracks->size() == 2);
    }

    SECTION("track delete notifications remove allTracks membership")
    {
      auto const trackId = libraryFixture.addTrack("Track 1");
      auto allTracks = ao::test::requireValue(cache.acquire(kAllTracksListId));
      cache.reloadAllTracks();
      REQUIRE(allTracks->size() == 1);
      auto spy = TrackSourceBatchSpy{allTracks.source()};
      auto writerFixture = LibraryWriterFixture{libraryFixture.library(), changes};
      auto& writer = writerFixture.writer();

      CHECK(writer.deleteTrack(trackId).has_value());
      CHECK(allTracks->size() == 0);
      REQUIRE(spy.batches.size() == 1);
      REQUIRE(spy.batches.front().deltas.size() == 1);
      auto const& removal = std::get<SourceRemoveRange>(spy.batches.front().deltas.front());
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
        listId =
          ao::test::requireValue(
            libraryFixture.library().lists().writer(transaction).create(ao::test::requireValue(builder.serialize())))
            .first;
        REQUIRE(transaction.commit());
      }

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
      smartListId =
        ao::test::requireValue(
          libraryFixture.library().lists().writer(transaction).create(ao::test::requireValue(builder.serialize())))
          .first;
      REQUIRE(transaction.commit());
    }

    auto changes = LibraryChanges{};
    auto writerFixture = LibraryWriterFixture{libraryFixture.library(), changes};
    auto cache = TrackSourceCache{libraryFixture.library(), changes};
    cache.reloadAllTracks();
    auto allTracksLease = ao::test::requireValue(cache.acquire(kAllTracksListId));
    auto smartLease = ao::test::requireValue(cache.acquire(smartListId));
    auto& smartSource = smartLease.source();
    REQUIRE(smartSource.size() == 0);

    auto allTracksBatches = std::vector<TrackSourceDeltaBatch>{};
    auto smartBatches = std::vector<TrackSourceDeltaBatch>{};
    auto allTracksSubscription =
      allTracksLease->subscribe([&](TrackSourceDeltaBatch const& batch) { allTracksBatches.push_back(batch); });
    auto smartSubscription =
      smartSource.subscribe([&](TrackSourceDeltaBatch const& batch) { smartBatches.push_back(batch); });

    auto const result = writerFixture.updateMetadata(std::array{trackId}, MetadataPatch{.optTitle = "After"});

    REQUIRE(result);
    REQUIRE(smartSource.size() == 1);
    CHECK(smartSource.trackIdAt(0) == trackId);
    REQUIRE(allTracksBatches.size() == 1);
    REQUIRE(allTracksBatches[0].deltas.size() == 1);
    auto const& update = std::get<SourceUpdateRange>(allTracksBatches[0].deltas[0]);
    CHECK(update.start == 0);
    CHECK(update.trackIds == std::vector{trackId});

    REQUIRE(smartBatches.size() == 1);
    REQUIRE(smartBatches[0].deltas.size() == 1);
    auto const& insert = std::get<SourceInsertRange>(smartBatches[0].deltas[0]);
    CHECK(insert.start == 0);
    CHECK(insert.trackIds == std::vector{trackId});
  }

  TEST_CASE("TrackSourceCache - shutdown does not semantically invalidate a leased All Tracks source",
            "[runtime][unit][source][track-source-cache]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const trackId = libraryFixture.addTrack("Pinned across cache shutdown");
    auto changes = LibraryChanges{};
    auto optLease = std::optional<TrackSourceLease>{};
    auto batches = std::vector<TrackSourceDeltaBatch>{};
    auto subscription = async::Subscription{};

    {
      auto cache = TrackSourceCache{libraryFixture.library(), changes};
      cache.reloadAllTracks();
      optLease.emplace(ao::test::requireValue(cache.acquire(kAllTracksListId)));
      subscription =
        (*optLease)->subscribe([&batches](TrackSourceDeltaBatch const& batch) { batches.push_back(batch); });

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
    auto changes = LibraryChanges{};
    auto writerFixture = LibraryWriterFixture{libraryFixture.library(), changes};
    auto& writer = writerFixture.writer();
    auto const listId = ao::test::requireValue(writer.createList(LibraryWriter::ListDraft{
      .kind = LibraryWriter::ListKind::Manual,
      .name = "Leased",
    }));
    auto cache = TrackSourceCache{libraryFixture.library(), changes};
    cache.reloadAllTracks();
    auto lease = ao::test::requireValue(cache.acquire(listId));
    auto* const identity = &lease.source();

    auto reacquired = ao::test::requireValue(cache.acquire(listId));
    CHECK(&reacquired.source() == identity);

    auto batches = std::vector<TrackSourceDeltaBatch>{};
    auto subscription = lease->subscribe([&](TrackSourceDeltaBatch const& batch) { batches.push_back(batch); });

    REQUIRE(writer.insertManualListTracks(listId, 0, std::array{trackId}));

    REQUIRE(lease->size() == 1);
    CHECK(lease->trackIdAt(0) == trackId);
    REQUIRE(batches.size() == 1);
    REQUIRE(batches[0].deltas.size() == 1);
    auto const& insert = std::get<SourceInsertRange>(batches[0].deltas[0]);
    CHECK(insert.start == 0);
    CHECK(insert.trackIds == std::vector{trackId});
    batches.clear();

    REQUIRE(writer.deleteList(listId));

    CHECK(lease->state() == TrackSourceState::Invalidated);
    REQUIRE(batches.size() == 1);
    CHECK(std::holds_alternative<SourceInvalidated>(batches[0].deltas[0]));
    auto const missingResult = cache.acquire(listId);
    REQUIRE_FALSE(missingResult);
    CHECK(missingResult.error().code == Error::Code::NotFound);
  }

  TEST_CASE("TrackSourceCache - same-id recreation creates a new identity beside the invalidated lease",
            "[runtime][unit][source][track-source-cache]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto changes = LibraryChanges{};
    auto writerFixture = LibraryWriterFixture{libraryFixture.library(), changes};
    auto& writer = writerFixture.writer();
    auto const listId = ao::test::requireValue(writer.createList(LibraryWriter::ListDraft{
      .kind = LibraryWriter::ListKind::Manual,
      .name = "Original",
    }));
    auto cache = TrackSourceCache{libraryFixture.library(), changes};
    auto oldLease = ao::test::requireValue(cache.acquire(listId));
    auto* const oldIdentity = &oldLease.source();
    auto oldBatches = std::vector<TrackSourceDeltaBatch>{};
    auto oldSubscription =
      oldLease->subscribe([&](TrackSourceDeltaBatch const& batch) { oldBatches.push_back(batch); });

    REQUIRE(writer.deleteList(listId));
    REQUIRE(oldLease->state() == TrackSourceState::Invalidated);
    REQUIRE(oldBatches.size() == 1);
    CHECK(std::holds_alternative<SourceInvalidated>(oldBatches[0].deltas[0]));

    auto const recreatedId = ao::test::requireValue(writer.createList(LibraryWriter::ListDraft{
      .kind = LibraryWriter::ListKind::Manual,
      .name = "Recreated",
    }));
    REQUIRE(recreatedId == listId);
    auto newLease = ao::test::requireValue(cache.acquire(recreatedId));

    CHECK(&newLease.source() != oldIdentity);
    CHECK(newLease->state() == TrackSourceState::Live);
    CHECK(oldLease->state() == TrackSourceState::Invalidated);
    CHECK(oldBatches.size() == 1);
  }

  TEST_CASE("TrackSourceCache - parent deletion invalidates its cached dependency graph once",
            "[runtime][unit][source][track-source-cache]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto changes = LibraryChanges{};
    auto writerFixture = LibraryWriterFixture{libraryFixture.library(), changes};
    auto& writer = writerFixture.writer();
    auto const parentId = ao::test::requireValue(writer.createList(LibraryWriter::ListDraft{
      .kind = LibraryWriter::ListKind::Manual,
      .name = "Parent",
    }));
    auto const childId = ao::test::requireValue(writer.createList(LibraryWriter::ListDraft{
      .kind = LibraryWriter::ListKind::Manual,
      .parentId = parentId,
      .name = "Child",
    }));
    auto cache = TrackSourceCache{libraryFixture.library(), changes};
    auto parentLease = ao::test::requireValue(cache.acquire(parentId));
    auto childLease = ao::test::requireValue(cache.acquire(childId));
    auto parentAgain = ao::test::requireValue(cache.acquire(parentId));
    CHECK(&parentAgain.source() == &parentLease.source());

    auto parentBatches = std::vector<TrackSourceDeltaBatch>{};
    auto childBatches = std::vector<TrackSourceDeltaBatch>{};
    auto parentSubscription =
      parentAgain->subscribe([&](TrackSourceDeltaBatch const& batch) { parentBatches.push_back(batch); });
    auto childSubscription =
      childLease->subscribe([&](TrackSourceDeltaBatch const& batch) { childBatches.push_back(batch); });

    REQUIRE(writer.deleteList(parentId));

    CHECK(parentAgain->state() == TrackSourceState::Invalidated);
    CHECK(childLease->state() == TrackSourceState::Invalidated);
    REQUIRE(parentBatches.size() == 1);
    REQUIRE(childBatches.size() == 1);
    CHECK(std::holds_alternative<SourceInvalidated>(parentBatches[0].deltas[0]));
    CHECK(std::holds_alternative<SourceInvalidated>(childBatches[0].deltas[0]));
  }

  TEST_CASE("TrackSourceCache - reparent rewires deletion propagation to the new parent",
            "[runtime][unit][source][track-source-cache]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto changes = LibraryChanges{};
    auto writerFixture = LibraryWriterFixture{libraryFixture.library(), changes};
    auto& writer = writerFixture.writer();
    auto const oldParentId = ao::test::requireValue(writer.createList(LibraryWriter::ListDraft{
      .kind = LibraryWriter::ListKind::Manual,
      .name = "Old parent",
    }));
    auto const newParentId = ao::test::requireValue(writer.createList(LibraryWriter::ListDraft{
      .kind = LibraryWriter::ListKind::Manual,
      .name = "New parent",
    }));
    auto const childId = ao::test::requireValue(writer.createList(LibraryWriter::ListDraft{
      .kind = LibraryWriter::ListKind::Manual,
      .parentId = oldParentId,
      .name = "Child",
    }));
    auto cache = TrackSourceCache{libraryFixture.library(), changes};
    auto childLease = ao::test::requireValue(cache.acquire(childId));
    auto* const childIdentity = &childLease.source();
    auto childBatches = std::vector<TrackSourceDeltaBatch>{};
    auto childSubscription =
      childLease->subscribe([&](TrackSourceDeltaBatch const& batch) { childBatches.push_back(batch); });

    REQUIRE(writer.updateList(LibraryWriter::ListDraft{
      .kind = LibraryWriter::ListKind::Manual,
      .parentId = newParentId,
      .listId = childId,
      .name = "Child",
    }));

    CHECK(&childLease.source() == childIdentity);
    CHECK(childLease->state() == TrackSourceState::Live);
    REQUIRE(childBatches.size() == 1);
    CHECK(std::holds_alternative<SourceReset>(childBatches[0].deltas[0]));

    REQUIRE(writer.deleteList(oldParentId));
    CHECK(childLease->state() == TrackSourceState::Live);
    CHECK(childBatches.size() == 1);

    REQUIRE(writer.deleteList(newParentId));
    CHECK(childLease->state() == TrackSourceState::Invalidated);
    REQUIRE(childBatches.size() == 2);
    CHECK(std::holds_alternative<SourceInvalidated>(childBatches[1].deltas[0]));
  }

  TEST_CASE("TrackSourceCache - definition rebind keeps identity and metadata-only updates emit nothing",
            "[runtime][unit][source][track-source-cache]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto changes = LibraryChanges{};
    auto writerFixture = LibraryWriterFixture{libraryFixture.library(), changes};
    auto& writer = writerFixture.writer();
    auto draft = LibraryWriter::ListDraft{
      .kind = LibraryWriter::ListKind::Manual,
      .name = "Before",
    };
    auto const listId = ao::test::requireValue(writer.createList(draft));
    auto cache = TrackSourceCache{libraryFixture.library(), changes};
    auto lease = ao::test::requireValue(cache.acquire(listId));
    auto* const identity = &lease.source();
    auto batches = std::vector<TrackSourceDeltaBatch>{};
    auto subscription = lease->subscribe([&](TrackSourceDeltaBatch const& batch) { batches.push_back(batch); });

    draft.listId = listId;
    draft.name = "Metadata only";
    REQUIRE(writer.updateList(draft));
    CHECK(batches.empty());

    draft.kind = LibraryWriter::ListKind::Smart;
    draft.expression = "true";
    REQUIRE(writer.updateList(draft));

    CHECK(&lease.source() == identity);
    CHECK(lease->state() == TrackSourceState::Live);
    REQUIRE(batches.size() == 1);
    CHECK(std::holds_alternative<SourceReset>(batches[0].deltas[0]));
  }

  TEST_CASE("TrackSourceCache - detailed manual operations publish exact batches without reset fallback",
            "[runtime][unit][source][track-source-cache]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const first = libraryFixture.addTrack("First");
    auto const second = libraryFixture.addTrack("Second");
    auto const third = libraryFixture.addTrack("Third");
    auto const inserted = libraryFixture.addTrack("Inserted");
    auto changes = LibraryChanges{};
    auto writerFixture = LibraryWriterFixture{libraryFixture.library(), changes};
    auto& writer = writerFixture.writer();
    auto const listId = ao::test::requireValue(writer.createList(LibraryWriter::ListDraft{
      .kind = LibraryWriter::ListKind::Manual,
      .name = "Detailed",
      .trackIds = {first, second, third},
    }));
    auto cache = TrackSourceCache{libraryFixture.library(), changes};
    cache.reloadAllTracks();
    auto lease = ao::test::requireValue(cache.acquire(listId));
    auto batches = std::vector<TrackSourceDeltaBatch>{};
    [[maybe_unused]] auto subscription =
      lease->subscribe([&batches](TrackSourceDeltaBatch const& batch) { batches.push_back(batch); });

    auto const moveResult = writer.moveManualListTracks(listId, std::array{second}, 0);
    REQUIRE(moveResult);
    REQUIRE(batches.size() == 1);
    REQUIRE(batches[0].deltas.size() == 2);
    auto const& moveRemoval = std::get<SourceRemoveRange>(batches[0].deltas[0]);
    auto const& moveInsertion = std::get<SourceInsertRange>(batches[0].deltas[1]);
    CHECK(moveRemoval.start == 1);
    CHECK(moveRemoval.trackIds == std::vector{second});
    CHECK(moveInsertion.start == 0);
    CHECK(moveInsertion.trackIds == std::vector{second});

    auto const insertResult = writer.insertManualListTracks(listId, 2, std::array{inserted});
    REQUIRE(insertResult);
    REQUIRE(batches.size() == 2);
    REQUIRE(batches[1].deltas.size() == 1);
    auto const& exactInsertion = std::get<SourceInsertRange>(batches[1].deltas[0]);
    CHECK(exactInsertion.start == 2);
    CHECK(exactInsertion.trackIds == std::vector{inserted});

    auto const removeResult = writer.removeManualListTracks(listId, std::array{first});
    REQUIRE(removeResult);
    REQUIRE(batches.size() == 3);
    REQUIRE(batches[2].deltas.size() == 1);
    auto const& exactRemoval = std::get<SourceRemoveRange>(batches[2].deltas[0]);
    CHECK(exactRemoval.start == 1);
    CHECK(exactRemoval.trackIds == std::vector{first});

    auto const expectedAfterDetailed = std::vector{second, inserted, third};
    CHECK(sourceTrackIds(lease.source()) == expectedAfterDetailed);

    auto const replacement = writer.updateList(LibraryWriter::ListDraft{
      .kind = LibraryWriter::ListKind::Manual,
      .listId = listId,
      .name = "Detailed",
      .trackIds = {third, second, inserted},
    });
    REQUIRE(replacement);
    REQUIRE(batches.size() == 4);
    REQUIRE(batches[3].deltas.size() == 1);
    CHECK(std::holds_alternative<SourceReset>(batches[3].deltas.front()));
    auto const expectedAfterReset = std::vector{third, second, inserted};
    CHECK(sourceTrackIds(lease.source()) == expectedAfterReset);

    for (auto const& batch : batches | std::views::take(3))
    {
      CHECK_FALSE(std::holds_alternative<SourceReset>(batch.deltas.front()));
    }
  }

  TEST_CASE("TrackSourceCache - reentrant metadata mutation is rejected during detailed publication",
            "[runtime][regression][source][manual-list]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const first = libraryFixture.addTrack("First");
    auto const inserted = libraryFixture.addTrack("Inserted");
    auto changes = LibraryChanges{};
    auto writerFixture = LibraryWriterFixture{libraryFixture.library(), changes};
    auto& writer = writerFixture.writer();
    auto const listId = ao::test::requireValue(writer.createList(LibraryWriter::ListDraft{
      .kind = LibraryWriter::ListKind::Manual,
      .name = "Before",
      .trackIds = {first},
    }));
    auto cache = TrackSourceCache{libraryFixture.library(), changes};
    cache.reloadAllTracks();
    auto lease = ao::test::requireValue(cache.acquire(listId));
    auto* const identity = &lease.source();
    auto batches = std::vector<TrackSourceDeltaBatch>{};
    bool handledInsertion = false;
    auto nestedError = Error::Code::Generic;
    [[maybe_unused]] auto subscription = lease->subscribe(
      [&](TrackSourceDeltaBatch const& batch)
      {
        batches.push_back(batch);

        if (handledInsertion)
        {
          return;
        }

        handledInsertion = true;
        auto const result = writer.updateList(LibraryWriter::ListDraft{
          .kind = LibraryWriter::ListKind::Manual,
          .listId = listId,
          .name = "Renamed while publishing",
          .trackIds = {first, inserted},
        });

        if (!result)
        {
          nestedError = result.error().code;
        }
      });

    auto const result = writer.insertManualListTracks(listId, 1, std::array{inserted});

    REQUIRE(result);
    CHECK(nestedError == Error::Code::InvalidState);
    CHECK(&lease.source() == identity);
    CHECK(lease->state() == TrackSourceState::Live);
    CHECK(sourceTrackIds(lease.source()) == std::vector{first, inserted});
    REQUIRE(batches.size() == 1);
    REQUIRE(batches.front().deltas.size() == 1);
    auto const& insertion = std::get<SourceInsertRange>(batches.front().deltas.front());
    CHECK(insertion.start == 1);
    CHECK(insertion.trackIds == std::vector{inserted});

    auto const transaction = libraryFixture.library().readTransaction();
    auto const optView = libraryFixture.library().lists().reader(transaction).get(listId);
    REQUIRE(optView);
    CHECK(optView->name() == "Before");
  }

  TEST_CASE("TrackSourceCache - reentrant reparent is rejected during detailed publication",
            "[runtime][regression][source][manual-list]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const first = libraryFixture.addTrack("First");
    auto const inserted = libraryFixture.addTrack("Inserted");
    auto changes = LibraryChanges{};
    auto writerFixture = LibraryWriterFixture{libraryFixture.library(), changes};
    auto& writer = writerFixture.writer();
    auto const oldParentId = ao::test::requireValue(writer.createList(LibraryWriter::ListDraft{
      .kind = LibraryWriter::ListKind::Manual,
      .name = "Old parent",
      .trackIds = {first, inserted},
    }));
    auto const newParentId = ao::test::requireValue(writer.createList(LibraryWriter::ListDraft{
      .kind = LibraryWriter::ListKind::Manual,
      .name = "New parent",
      .trackIds = {inserted},
    }));
    auto const childId = ao::test::requireValue(writer.createList(LibraryWriter::ListDraft{
      .kind = LibraryWriter::ListKind::Manual,
      .parentId = oldParentId,
      .name = "Child",
      .trackIds = {first},
    }));
    auto cache = TrackSourceCache{libraryFixture.library(), changes};
    cache.reloadAllTracks();
    auto childLease = ao::test::requireValue(cache.acquire(childId));
    auto* const identity = &childLease.source();
    auto batches = std::vector<TrackSourceDeltaBatch>{};
    auto trailingBatches = std::vector<TrackSourceDeltaBatch>{};
    auto snapshotAfterNestedUpdate = std::vector<TrackId>{};
    bool handledInsertion = false;
    auto nestedError = Error::Code::Generic;
    [[maybe_unused]] auto subscription = childLease->subscribe(
      [&](TrackSourceDeltaBatch const& batch)
      {
        batches.push_back(batch);

        if (handledInsertion)
        {
          return;
        }

        handledInsertion = true;
        auto const result = writer.updateList(LibraryWriter::ListDraft{
          .kind = LibraryWriter::ListKind::Manual,
          .parentId = newParentId,
          .listId = childId,
          .name = "Child",
          .trackIds = {first, inserted},
        });

        if (!result)
        {
          nestedError = result.error().code;
        }

        snapshotAfterNestedUpdate = sourceTrackIds(childLease.source());
      });
    [[maybe_unused]] auto trailingSubscription =
      childLease->subscribe([&](TrackSourceDeltaBatch const& batch) { trailingBatches.push_back(batch); });

    auto const result = writer.insertManualListTracks(childId, 1, std::array{inserted});

    REQUIRE(result);
    CHECK(nestedError == Error::Code::InvalidState);
    CHECK(snapshotAfterNestedUpdate == std::vector{first, inserted});
    CHECK(&childLease.source() == identity);
    CHECK(childLease->state() == TrackSourceState::Live);
    CHECK(sourceTrackIds(childLease.source()) == std::vector{first, inserted});
    REQUIRE(batches.size() == 1);
    REQUIRE(batches[0].deltas.size() == 1);
    auto const& insertion = std::get<SourceInsertRange>(batches[0].deltas.front());
    CHECK(insertion.start == 1);
    CHECK(insertion.trackIds == std::vector{inserted});
    REQUIRE(trailingBatches.size() == 1);
    CHECK(std::holds_alternative<SourceInsertRange>(trailingBatches[0].deltas.front()));

    {
      auto const transaction = libraryFixture.library().readTransaction();
      auto const optView = libraryFixture.library().lists().reader(transaction).get(childId);
      REQUIRE(optView);
      CHECK(optView->parentId() == oldParentId);
    }

    REQUIRE(writer.deleteList(newParentId));
    CHECK(childLease->state() == TrackSourceState::Live);
    CHECK(batches.size() == 1);

    REQUIRE(writer.deleteList(oldParentId));
    CHECK(childLease->state() == TrackSourceState::Invalidated);
    REQUIRE(batches.size() == 2);
    REQUIRE(batches[1].deltas.size() == 1);
    CHECK(std::holds_alternative<SourceInvalidated>(batches[1].deltas.front()));
  }

  TEST_CASE("TrackSourceCache - reentrant mutations are rejected before an observer exception faults authoring",
            "[runtime][regression][source][manual-list]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const first = libraryFixture.addTrack("First");
    auto const second = libraryFixture.addTrack("Second");
    auto const third = libraryFixture.addTrack("Third");
    auto changes = LibraryChanges{};
    auto writerFixture = LibraryWriterFixture{libraryFixture.library(), changes};
    auto& writer = writerFixture.writer();
    auto const oldParentId = ao::test::requireValue(writer.createList(LibraryWriter::ListDraft{
      .kind = LibraryWriter::ListKind::Manual,
      .name = "Old parent",
      .trackIds = {first, second, third},
    }));
    auto const intermediateParentId = ao::test::requireValue(writer.createList(LibraryWriter::ListDraft{
      .kind = LibraryWriter::ListKind::Manual,
      .name = "Intermediate parent",
      .trackIds = {first, second},
    }));
    auto const finalParentId = ao::test::requireValue(writer.createList(LibraryWriter::ListDraft{
      .kind = LibraryWriter::ListKind::Manual,
      .name = "Final parent",
      .trackIds = {third},
    }));
    auto const childId = ao::test::requireValue(writer.createList(LibraryWriter::ListDraft{
      .kind = LibraryWriter::ListKind::Manual,
      .parentId = oldParentId,
      .name = "Child",
      .trackIds = {first},
    }));
    auto cache = TrackSourceCache{libraryFixture.library(), changes};
    cache.reloadAllTracks();
    auto childLease = ao::test::requireValue(cache.acquire(childId));
    auto* const identity = &childLease.source();
    auto batches = std::vector<TrackSourceDeltaBatch>{};
    bool callbackInvoked = false;
    auto nestedInsertError = Error::Code::Generic;
    auto intermediateReparentError = Error::Code::Generic;
    auto finalReparentError = Error::Code::Generic;
    [[maybe_unused]] auto subscription = childLease->subscribe(
      [&](TrackSourceDeltaBatch const& batch)
      {
        batches.push_back(batch);

        if (callbackInvoked)
        {
          return;
        }

        callbackInvoked = true;
        auto const nestedInsertResult = writer.insertManualListTracks(childId, 2, std::array{third});

        if (!nestedInsertResult)
        {
          nestedInsertError = nestedInsertResult.error().code;
        }

        auto const intermediateResult = writer.updateList(LibraryWriter::ListDraft{
          .kind = LibraryWriter::ListKind::Manual,
          .parentId = intermediateParentId,
          .listId = childId,
          .name = "Child",
          .trackIds = {first, second, third},
        });

        if (!intermediateResult)
        {
          intermediateReparentError = intermediateResult.error().code;
        }

        auto const finalResult = writer.updateList(LibraryWriter::ListDraft{
          .kind = LibraryWriter::ListKind::Manual,
          .parentId = finalParentId,
          .listId = childId,
          .name = "Child",
          .trackIds = {first, second, third},
        });

        if (!finalResult)
        {
          finalReparentError = finalResult.error().code;
        }

        throwException<Exception>("reentrant observer failure");
      });

    CHECK_THROWS_WITH(writer.insertManualListTracks(childId, 1, std::array{second}), "reentrant observer failure");

    CHECK(nestedInsertError == Error::Code::InvalidState);
    CHECK(intermediateReparentError == Error::Code::InvalidState);
    CHECK(finalReparentError == Error::Code::InvalidState);
    CHECK(&childLease.source() == identity);
    CHECK(childLease->state() == TrackSourceState::Live);
    CHECK(sourceTrackIds(childLease.source()) == std::vector{first, second});
    REQUIRE(batches.size() == 1);
    REQUIRE(batches[0].deltas.size() == 1);
    auto const& outerInsertion = std::get<SourceInsertRange>(batches[0].deltas.front());
    CHECK(outerInsertion.start == 1);
    CHECK(outerInsertion.trackIds == std::vector{second});

    {
      auto const transaction = libraryFixture.library().readTransaction();
      auto const optView = libraryFixture.library().lists().reader(transaction).get(childId);
      REQUIRE(optView);
      CHECK(optView->parentId() == oldParentId);
    }

    auto const rejectedAfterFault = writer.deleteList(finalParentId);
    REQUIRE_FALSE(rejectedAfterFault);
    CHECK(rejectedAfterFault.error().code == Error::Code::InvalidState);
  }

  TEST_CASE("TrackSourceCache - hidden manual insert does not publish and re-enters in stored order",
            "[runtime][unit][source][track-source-cache]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const visible = libraryFixture.addTrack("Visible");
    auto const hidden = libraryFixture.addTrack("Parent hidden");
    auto changes = LibraryChanges{};
    auto writerFixture = LibraryWriterFixture{libraryFixture.library(), changes};
    auto& writer = writerFixture.writer();
    auto const parentId = ao::test::requireValue(writer.createList(LibraryWriter::ListDraft{
      .kind = LibraryWriter::ListKind::Manual,
      .name = "Parent",
      .trackIds = {visible},
    }));
    auto const childId = ao::test::requireValue(writer.createList(LibraryWriter::ListDraft{
      .kind = LibraryWriter::ListKind::Manual,
      .parentId = parentId,
      .name = "Child",
      .trackIds = {visible},
    }));
    auto cache = TrackSourceCache{libraryFixture.library(), changes};
    cache.reloadAllTracks();
    auto childLease = ao::test::requireValue(cache.acquire(childId));
    auto batches = std::vector<TrackSourceDeltaBatch>{};
    [[maybe_unused]] auto subscription =
      childLease->subscribe([&batches](TrackSourceDeltaBatch const& batch) { batches.push_back(batch); });

    auto const hiddenInsert = writer.insertManualListTracks(childId, 1, std::array{hidden});
    REQUIRE(hiddenInsert);
    CHECK(batches.empty());
    CHECK(sourceTrackIds(childLease.source()) == std::vector{visible});

    auto const parentInsert = writer.insertManualListTracks(parentId, 0, std::array{hidden});
    REQUIRE(parentInsert);

    auto const expected = std::vector{visible, hidden};
    REQUIRE(batches.size() == 1);
    REQUIRE(batches.front().deltas.size() == 1);
    auto const& insertion = std::get<SourceInsertRange>(batches.front().deltas.front());
    CHECK(insertion.start == 1);
    CHECK(insertion.trackIds == std::vector{hidden});
    CHECK(sourceTrackIds(childLease.source()) == expected);
  }

  TEST_CASE("TrackSourceCache - track deletion does not duplicate detailed manual removal via parent",
            "[runtime][unit][source][track-source-cache]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const first = libraryFixture.addTrack("First");
    auto const deleted = libraryFixture.addTrack("Deleted");
    auto const third = libraryFixture.addTrack("Third");
    auto changes = LibraryChanges{};
    auto writerFixture = LibraryWriterFixture{libraryFixture.library(), changes};
    auto& writer = writerFixture.writer();
    auto const listId = ao::test::requireValue(writer.createList(LibraryWriter::ListDraft{
      .kind = LibraryWriter::ListKind::Manual,
      .name = "Delete target",
      .trackIds = {first, deleted, third},
    }));
    auto cache = TrackSourceCache{libraryFixture.library(), changes};
    cache.reloadAllTracks();
    auto lease = ao::test::requireValue(cache.acquire(listId));
    auto batches = std::vector<TrackSourceDeltaBatch>{};
    [[maybe_unused]] auto subscription =
      lease->subscribe([&batches](TrackSourceDeltaBatch const& batch) { batches.push_back(batch); });

    auto const result = writer.deleteTrack(deleted);

    REQUIRE(result);
    auto const expected = std::vector{first, third};
    REQUIRE(batches.size() == 1);
    REQUIRE(batches.front().deltas.size() == 1);
    auto const& removal = std::get<SourceRemoveRange>(batches.front().deltas.front());
    CHECK(removal.start == 1);
    CHECK(removal.trackIds == std::vector{deleted});
    CHECK(sourceTrackIds(lease.source()) == expected);
  }

  TEST_CASE("TrackSourceCache - identical source specs share one ad-hoc source", "[runtime][unit][source][source-spec]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    libraryFixture.addTrack("First");
    auto changes = LibraryChanges{};
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
} // namespace ao::rt::test
