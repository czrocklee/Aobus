// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/runtime/source/ManualListSourceTestSupport.h"
#include "test/unit/runtime/source/TrackSourceTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/source/ManualListSource.h>
#include <ao/rt/source/TrackSourceDelta.h>
#include <ao/rt/source/TrackSourceLease.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <optional>
#include <variant>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    std::vector<TrackId> storedTrackIdsOf(ManualListSource const& source)
    {
      auto const trackIds = source.storedTrackIds();
      return {trackIds.begin(), trackIds.end()};
    }
  } // namespace

  TEST_CASE("ManualListSource - keeps stored order separate from effective parent membership",
            "[runtime][unit][source][manual-list]")
  {
    auto parentPtr = makeMutableTrackSource({TrackId{3}, TrackId{1}});
    auto view = ListViewOwner{{TrackId{1}, TrackId{2}, TrackId{3}}};
    auto source = ManualListSource{view.view(), TrackSourceLease{parentPtr}};
    auto const expectedStored = std::vector{TrackId{1}, TrackId{2}, TrackId{3}};
    auto const expectedEffective = std::vector{TrackId{1}, TrackId{3}};

    CHECK(storedTrackIdsOf(source) == expectedStored);
    CHECK(sourceTrackIds(source) == expectedEffective);
    CHECK(source.size() == 2);
    CHECK(source.trackIdAt(1) == TrackId{3});
    CHECK(source.indexOf(TrackId{1}) == std::optional<std::size_t>{0});
    CHECK(source.indexOf(TrackId{3}) == std::optional<std::size_t>{1});
    CHECK(source.indexOf(TrackId{2}) == std::nullopt);
    CHECK(source.contains(TrackId{3}));
    CHECK_FALSE(source.contains(TrackId{2}));
  }

  TEST_CASE("ManualListSource - hidden-only reload updates stored order without a source batch",
            "[runtime][unit][source][manual-list]")
  {
    auto parentPtr = makeMutableTrackSource({TrackId{1}});
    auto initialView = ListViewOwner{{TrackId{1}, TrackId{2}}};
    auto source = ManualListSource{initialView.view(), TrackSourceLease{parentPtr}};
    auto batches = std::vector<TrackSourceDeltaBatch>{};
    [[maybe_unused]] auto subscription =
      source.subscribe([&batches](TrackSourceDeltaBatch const& batch) { batches.push_back(batch); });

    auto replacementView = ListViewOwner{{TrackId{1}, TrackId{2}, TrackId{3}}};
    source.reloadFromListView(replacementView.view());

    auto const expectedStored = std::vector{TrackId{1}, TrackId{2}, TrackId{3}};
    auto const expectedEffective = std::vector{TrackId{1}};
    CHECK(storedTrackIdsOf(source) == expectedStored);
    CHECK(sourceTrackIds(source) == expectedEffective);
    CHECK(batches.empty());
  }

  TEST_CASE("ManualListSource - visible reload publishes one reset after installing final state",
            "[runtime][unit][source][manual-list]")
  {
    auto parentPtr = makeMutableTrackSource({TrackId{1}, TrackId{3}});
    auto initialView = ListViewOwner{{TrackId{1}}};
    auto source = ManualListSource{initialView.view(), TrackSourceLease{parentPtr}};
    auto batches = std::vector<TrackSourceDeltaBatch>{};
    auto observedSnapshots = std::vector<std::vector<TrackId>>{};
    [[maybe_unused]] auto subscription = source.subscribe(
      [&source, &batches, &observedSnapshots](TrackSourceDeltaBatch const& batch)
      {
        batches.push_back(batch);
        observedSnapshots.push_back(sourceTrackIds(source));
      });

    auto replacementView = ListViewOwner{{TrackId{3}, TrackId{2}, TrackId{1}}};
    source.reloadFromListView(replacementView.view());

    auto const expectedEffective = std::vector{TrackId{3}, TrackId{1}};
    REQUIRE(batches.size() == 1);
    REQUIRE(batches.front().deltas.size() == 1);
    CHECK(std::holds_alternative<SourceReset>(batches.front().deltas.front()));
    CHECK(sourceTrackIds(source) == expectedEffective);
    REQUIRE(observedSnapshots.size() == 1);
    CHECK(observedSnapshots.front() == expectedEffective);
  }

  TEST_CASE("ManualListSource - exact insert emits only visible identities", "[runtime][unit][source][manual-list]")
  {
    auto parentPtr = makeMutableTrackSource({TrackId{1}, TrackId{3}});
    auto view = ListViewOwner{{TrackId{1}}};
    auto source = ManualListSource{view.view(), TrackSourceLease{parentPtr}};
    auto batches = std::vector<TrackSourceDeltaBatch>{};
    [[maybe_unused]] auto subscription =
      source.subscribe([&batches](TrackSourceDeltaBatch const& batch) { batches.push_back(batch); });

    source.applyManualTracksInsert(ManualTracksInsert{.storedIndex = 1, .trackIds = {TrackId{2}, TrackId{3}}});

    auto const expectedStored = std::vector{TrackId{1}, TrackId{2}, TrackId{3}};
    auto const expectedEffective = std::vector{TrackId{1}, TrackId{3}};
    REQUIRE(batches.size() == 1);
    REQUIRE(batches.front().deltas.size() == 1);
    auto const& insertion = std::get<SourceInsertRange>(batches.front().deltas.front());
    CHECK(insertion.start == 1);
    CHECK(insertion.trackIds == std::vector{TrackId{3}});
    CHECK(storedTrackIdsOf(source) == expectedStored);
    CHECK(sourceTrackIds(source) == expectedEffective);

    source.applyManualTracksInsert(ManualTracksInsert{.storedIndex = 2, .trackIds = {TrackId{4}}});

    auto const expectedStoredAfterHiddenInsert = std::vector{TrackId{1}, TrackId{2}, TrackId{4}, TrackId{3}};
    CHECK(storedTrackIdsOf(source) == expectedStoredAfterHiddenInsert);
    CHECK(sourceTrackIds(source) == expectedEffective);
    CHECK(batches.size() == 1);

    source.applyManualTracksRemove(
      ManualTracksRemove{.removals = {{.start = 1, .trackIds = {TrackId{2}, TrackId{4}}}}});

    auto const expectedStoredAfterHiddenRemove = std::vector{TrackId{1}, TrackId{3}};
    CHECK(storedTrackIdsOf(source) == expectedStoredAfterHiddenRemove);
    CHECK(sourceTrackIds(source) == expectedEffective);
    CHECK(batches.size() == 1);
  }

  TEST_CASE("ManualListSource - exact remove applies descending stored ranges and visible delta",
            "[runtime][unit][source][manual-list]")
  {
    auto parentPtr = makeMutableTrackSource({TrackId{1}, TrackId{3}, TrackId{4}});
    auto view = ListViewOwner{{TrackId{1}, TrackId{2}, TrackId{3}, TrackId{4}, TrackId{5}}};
    auto source = ManualListSource{view.view(), TrackSourceLease{parentPtr}};
    auto batches = std::vector<TrackSourceDeltaBatch>{};
    [[maybe_unused]] auto subscription =
      source.subscribe([&batches](TrackSourceDeltaBatch const& batch) { batches.push_back(batch); });

    source.applyManualTracksRemove(ManualTracksRemove{
      .removals = {{.start = 4, .trackIds = {TrackId{5}}}, {.start = 1, .trackIds = {TrackId{2}, TrackId{3}}}}});

    auto const expectedStored = std::vector{TrackId{1}, TrackId{4}};
    auto const expectedEffective = std::vector{TrackId{1}, TrackId{4}};
    REQUIRE(batches.size() == 1);
    REQUIRE(batches.front().deltas.size() == 1);
    auto const& removal = std::get<SourceRemoveRange>(batches.front().deltas.front());
    CHECK(removal.start == 1);
    CHECK(removal.trackIds == std::vector{TrackId{3}});
    CHECK(storedTrackIdsOf(source) == expectedStored);
    CHECK(sourceTrackIds(source) == expectedEffective);
  }

  TEST_CASE("ManualListSource - exact move preserves known identity through hidden selections",
            "[runtime][unit][source][manual-list]")
  {
    auto parentPtr = makeMutableTrackSource({TrackId{1}, TrackId{3}, TrackId{4}});
    auto view = ListViewOwner{{TrackId{1}, TrackId{2}, TrackId{3}, TrackId{4}, TrackId{5}}};
    auto source = ManualListSource{view.view(), TrackSourceLease{parentPtr}};
    auto batches = std::vector<TrackSourceDeltaBatch>{};
    [[maybe_unused]] auto subscription =
      source.subscribe([&batches](TrackSourceDeltaBatch const& batch) { batches.push_back(batch); });

    source.applyManualTracksMove(ManualTracksMove{
      .removals = {{.start = 4, .trackIds = {TrackId{5}}}, {.start = 2, .trackIds = {TrackId{3}}}},
      .insertionIndexAfterRemoval = 0,
      .insertedTrackIds = {TrackId{3}, TrackId{5}},
    });

    auto const expectedStored = std::vector{TrackId{3}, TrackId{5}, TrackId{1}, TrackId{2}, TrackId{4}};
    auto const expectedEffective = std::vector{TrackId{3}, TrackId{1}, TrackId{4}};
    REQUIRE(batches.size() == 1);
    REQUIRE(batches.front().deltas.size() == 2);
    auto const& removal = std::get<SourceRemoveRange>(batches.front().deltas[0]);
    auto const& insertion = std::get<SourceInsertRange>(batches.front().deltas[1]);
    CHECK(removal.start == 1);
    CHECK(removal.trackIds == std::vector{TrackId{3}});
    CHECK(insertion.start == 0);
    CHECK(insertion.trackIds == std::vector{TrackId{3}});
    CHECK(storedTrackIdsOf(source) == expectedStored);
    CHECK(sourceTrackIds(source) == expectedEffective);
  }

  TEST_CASE("ManualListSource - preserves exact move identity for an ambiguous final permutation",
            "[runtime][regression][source][manual-list]")
  {
    auto parentPtr = makeMutableTrackSource({TrackId{1}, TrackId{2}, TrackId{3}});
    auto view = ListViewOwner{{TrackId{1}, TrackId{2}, TrackId{3}}};
    auto moveFirstToEnd = ManualListSource{view.view(), TrackSourceLease{parentPtr}};
    auto moveTailToFront = ManualListSource{view.view(), TrackSourceLease{parentPtr}};
    auto firstMoveBatches = std::vector<TrackSourceDeltaBatch>{};
    auto tailMoveBatches = std::vector<TrackSourceDeltaBatch>{};
    [[maybe_unused]] auto firstMoveSubscription = moveFirstToEnd.subscribe(
      [&firstMoveBatches](TrackSourceDeltaBatch const& batch) { firstMoveBatches.push_back(batch); });
    [[maybe_unused]] auto tailMoveSubscription = moveTailToFront.subscribe(
      [&tailMoveBatches](TrackSourceDeltaBatch const& batch) { tailMoveBatches.push_back(batch); });

    moveFirstToEnd.applyManualTracksMove(ManualTracksMove{
      .removals = {{.start = 0, .trackIds = {TrackId{1}}}},
      .insertionIndexAfterRemoval = 2,
      .insertedTrackIds = {TrackId{1}},
    });
    moveTailToFront.applyManualTracksMove(ManualTracksMove{
      .removals = {{.start = 1, .trackIds = {TrackId{2}, TrackId{3}}}},
      .insertionIndexAfterRemoval = 0,
      .insertedTrackIds = {TrackId{2}, TrackId{3}},
    });

    auto const expected = std::vector{TrackId{2}, TrackId{3}, TrackId{1}};
    CHECK(storedTrackIdsOf(moveFirstToEnd) == expected);
    CHECK(sourceTrackIds(moveFirstToEnd) == expected);
    CHECK(storedTrackIdsOf(moveTailToFront) == expected);
    CHECK(sourceTrackIds(moveTailToFront) == expected);

    REQUIRE(firstMoveBatches.size() == 1);
    REQUIRE(firstMoveBatches.front().deltas.size() == 2);
    REQUIRE(std::holds_alternative<SourceRemoveRange>(firstMoveBatches.front().deltas[0]));
    REQUIRE(std::holds_alternative<SourceInsertRange>(firstMoveBatches.front().deltas[1]));
    auto const& firstRemoval = std::get<SourceRemoveRange>(firstMoveBatches.front().deltas[0]);
    auto const& firstInsertion = std::get<SourceInsertRange>(firstMoveBatches.front().deltas[1]);
    CHECK(firstRemoval.start == 0);
    CHECK(firstRemoval.trackIds == std::vector{TrackId{1}});
    CHECK(firstInsertion.start == 2);
    CHECK(firstInsertion.trackIds == std::vector{TrackId{1}});

    REQUIRE(tailMoveBatches.size() == 1);
    REQUIRE(tailMoveBatches.front().deltas.size() == 2);
    REQUIRE(std::holds_alternative<SourceRemoveRange>(tailMoveBatches.front().deltas[0]));
    REQUIRE(std::holds_alternative<SourceInsertRange>(tailMoveBatches.front().deltas[1]));
    auto const& tailRemoval = std::get<SourceRemoveRange>(tailMoveBatches.front().deltas[0]);
    auto const& tailInsertion = std::get<SourceInsertRange>(tailMoveBatches.front().deltas[1]);
    CHECK(tailRemoval.start == 1);
    CHECK(tailRemoval.trackIds == std::vector{TrackId{2}, TrackId{3}});
    CHECK(tailInsertion.start == 0);
    CHECK(tailInsertion.trackIds == std::vector{TrackId{2}, TrackId{3}});
  }

  TEST_CASE("ManualListSource - hidden-only move changes stored order without a source batch",
            "[runtime][unit][source][manual-list]")
  {
    auto parentPtr = makeMutableTrackSource({TrackId{1}});
    auto view = ListViewOwner{{TrackId{1}, TrackId{2}, TrackId{3}}};
    auto source = ManualListSource{view.view(), TrackSourceLease{parentPtr}};
    auto batches = std::vector<TrackSourceDeltaBatch>{};
    [[maybe_unused]] auto subscription =
      source.subscribe([&batches](TrackSourceDeltaBatch const& batch) { batches.push_back(batch); });

    source.applyManualTracksMove(ManualTracksMove{
      .removals = {{.start = 2, .trackIds = {TrackId{3}}}},
      .insertionIndexAfterRemoval = 0,
      .insertedTrackIds = {TrackId{3}},
    });

    auto const expectedStored = std::vector{TrackId{3}, TrackId{1}, TrackId{2}};
    auto const expectedEffective = std::vector{TrackId{1}};
    CHECK(storedTrackIdsOf(source) == expectedStored);
    CHECK(sourceTrackIds(source) == expectedEffective);
    CHECK(batches.empty());
  }
} // namespace ao::rt::test
