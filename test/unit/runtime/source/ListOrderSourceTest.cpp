// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/runtime/source/ListOrderSourceTestSupport.h"
#include "test/unit/runtime/source/TrackSourceTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/rt/TrackEditScript.h>
#include <ao/rt/source/ListOrderSource.h>
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
    std::vector<TrackId> orderTrackIdsOf(ListOrderSource const& source)
    {
      auto const trackIds = source.orderTrackIds();
      return {trackIds.begin(), trackIds.end()};
    }
  } // namespace

  TEST_CASE("ListOrderSource - keeps stored order separate from effective parent membership",
            "[runtime][unit][source][list-order]")
  {
    auto parentPtr = makeMutableTrackSource({TrackId{3}, TrackId{1}});
    auto view = ListViewOwner{{TrackId{1}, TrackId{2}, TrackId{3}}};
    auto source = ListOrderSource{view.view().orderTrackIds(), TrackSourceLease{parentPtr}};
    auto const expectedStored = std::vector{TrackId{1}, TrackId{2}, TrackId{3}};
    auto const expectedEffective = std::vector{TrackId{1}, TrackId{3}};

    CHECK(orderTrackIdsOf(source) == expectedStored);
    CHECK(sourceTrackIds(source) == expectedEffective);
    CHECK(source.size() == 2);
    CHECK(source.trackIdAt(1) == TrackId{3});
    CHECK(source.indexOf(TrackId{1}) == std::optional<std::size_t>{0});
    CHECK(source.indexOf(TrackId{3}) == std::optional<std::size_t>{1});
    CHECK(source.indexOf(TrackId{2}) == std::nullopt);
    CHECK(source.contains(TrackId{3}));
    CHECK_FALSE(source.contains(TrackId{2}));
  }

  TEST_CASE("ListOrderSource - materializing existing effective identities emits no batch",
            "[runtime][unit][source][list-order]")
  {
    auto parentPtr = makeMutableTrackSource({TrackId{1}, TrackId{3}});
    auto view = ListViewOwner{{TrackId{1}}};
    auto source = ListOrderSource{view.view().orderTrackIds(), TrackSourceLease{parentPtr}};
    auto batches = std::vector<TrackSourceDelta>{};
    [[maybe_unused]] auto subscription =
      source.subscribe([&batches](TrackSourceDelta const& batch) noexcept { batches.push_back(batch); });

    source.applyOrderEditScript(delta::RegularTrackEditScript{
      .edits = {delta::InsertRange{.start = 1, .trackIds = {TrackId{2}, TrackId{3}}}},
    });

    auto const expectedStored = std::vector{TrackId{1}, TrackId{2}, TrackId{3}};
    auto const expectedEffective = std::vector{TrackId{1}, TrackId{3}};
    CHECK(batches.empty());
    CHECK(orderTrackIdsOf(source) == expectedStored);
    CHECK(sourceTrackIds(source) == expectedEffective);

    source.applyOrderEditScript(delta::RegularTrackEditScript{
      .edits = {delta::InsertRange{.start = 2, .trackIds = {TrackId{4}}}},
    });

    auto const expectedStoredAfterHiddenInsert = std::vector{TrackId{1}, TrackId{2}, TrackId{4}, TrackId{3}};
    CHECK(orderTrackIdsOf(source) == expectedStoredAfterHiddenInsert);
    CHECK(sourceTrackIds(source) == expectedEffective);
    CHECK(batches.empty());

    source.applyOrderEditScript(delta::RegularTrackEditScript{
      .edits = {delta::RemoveRange{.start = 1, .trackIds = {TrackId{2}, TrackId{4}}}},
    });

    auto const expectedStoredAfterHiddenRemove = std::vector{TrackId{1}, TrackId{3}};
    CHECK(orderTrackIdsOf(source) == expectedStoredAfterHiddenRemove);
    CHECK(sourceTrackIds(source) == expectedEffective);
    CHECK(batches.empty());
  }

  TEST_CASE("ListOrderSource - exact remove applies descending stored ranges and visible delta",
            "[runtime][unit][source][list-order]")
  {
    auto parentPtr = makeMutableTrackSource({TrackId{1}, TrackId{3}, TrackId{4}});
    auto view = ListViewOwner{{TrackId{1}, TrackId{2}, TrackId{3}, TrackId{4}, TrackId{5}}};
    auto source = ListOrderSource{view.view().orderTrackIds(), TrackSourceLease{parentPtr}};
    auto batches = std::vector<TrackSourceDelta>{};
    [[maybe_unused]] auto subscription =
      source.subscribe([&batches](TrackSourceDelta const& batch) noexcept { batches.push_back(batch); });

    source.applyOrderEditScript(delta::RegularTrackEditScript{
      .edits = {delta::RemoveRange{.start = 4, .trackIds = {TrackId{5}}},
                delta::RemoveRange{.start = 1, .trackIds = {TrackId{2}, TrackId{3}}}},
    });

    auto const expectedStored = std::vector{TrackId{1}, TrackId{4}};
    auto const expectedEffective = std::vector{TrackId{1}, TrackId{4}, TrackId{3}};
    REQUIRE(batches.size() == 1);
    REQUIRE(sourceEditScript(batches.front()).edits.size() == 2);
    auto const& removal = std::get<delta::RemoveRange>(sourceEditScript(batches.front()).edits[0]);
    auto const& insertion = std::get<delta::InsertRange>(sourceEditScript(batches.front()).edits[1]);
    CHECK(removal.start == 1);
    CHECK(removal.trackIds == std::vector{TrackId{3}});
    CHECK(insertion.start == 2);
    CHECK(insertion.trackIds == std::vector{TrackId{3}});
    CHECK(orderTrackIdsOf(source) == expectedStored);
    CHECK(sourceTrackIds(source) == expectedEffective);
  }

  TEST_CASE("ListOrderSource - exact move preserves known identity through hidden selections",
            "[runtime][unit][source][list-order]")
  {
    auto parentPtr = makeMutableTrackSource({TrackId{1}, TrackId{3}, TrackId{4}});
    auto view = ListViewOwner{{TrackId{1}, TrackId{2}, TrackId{3}, TrackId{4}, TrackId{5}}};
    auto source = ListOrderSource{view.view().orderTrackIds(), TrackSourceLease{parentPtr}};
    auto batches = std::vector<TrackSourceDelta>{};
    [[maybe_unused]] auto subscription =
      source.subscribe([&batches](TrackSourceDelta const& batch) noexcept { batches.push_back(batch); });

    source.applyOrderEditScript(delta::RegularTrackEditScript{
      .edits = {delta::RemoveRange{.start = 4, .trackIds = {TrackId{5}}},
                delta::RemoveRange{.start = 2, .trackIds = {TrackId{3}}},
                delta::InsertRange{.start = 0, .trackIds = {TrackId{3}, TrackId{5}}}},
    });

    auto const expectedStored = std::vector{TrackId{3}, TrackId{5}, TrackId{1}, TrackId{2}, TrackId{4}};
    auto const expectedEffective = std::vector{TrackId{3}, TrackId{1}, TrackId{4}};
    REQUIRE(batches.size() == 1);
    REQUIRE(sourceEditScript(batches.front()).edits.size() == 2);
    auto const& removal = std::get<delta::RemoveRange>(sourceEditScript(batches.front()).edits[0]);
    auto const& insertion = std::get<delta::InsertRange>(sourceEditScript(batches.front()).edits[1]);
    CHECK(removal.start == 1);
    CHECK(removal.trackIds == std::vector{TrackId{3}});
    CHECK(insertion.start == 0);
    CHECK(insertion.trackIds == std::vector{TrackId{3}});
    CHECK(orderTrackIdsOf(source) == expectedStored);
    CHECK(sourceTrackIds(source) == expectedEffective);
  }

  TEST_CASE("ListOrderSource - preserves exact move identity for an ambiguous final permutation",
            "[runtime][regression][source][list-order]")
  {
    auto parentPtr = makeMutableTrackSource({TrackId{1}, TrackId{2}, TrackId{3}});
    auto view = ListViewOwner{{TrackId{1}, TrackId{2}, TrackId{3}}};
    auto moveFirstToEnd = ListOrderSource{view.view().orderTrackIds(), TrackSourceLease{parentPtr}};
    auto moveTailToFront = ListOrderSource{view.view().orderTrackIds(), TrackSourceLease{parentPtr}};
    auto firstMoveBatches = std::vector<TrackSourceDelta>{};
    auto tailMoveBatches = std::vector<TrackSourceDelta>{};
    [[maybe_unused]] auto firstMoveSubscription = moveFirstToEnd.subscribe(
      [&firstMoveBatches](TrackSourceDelta const& batch) noexcept { firstMoveBatches.push_back(batch); });
    [[maybe_unused]] auto tailMoveSubscription = moveTailToFront.subscribe(
      [&tailMoveBatches](TrackSourceDelta const& batch) noexcept { tailMoveBatches.push_back(batch); });

    moveFirstToEnd.applyOrderEditScript(delta::RegularTrackEditScript{
      .edits = {delta::RemoveRange{.start = 0, .trackIds = {TrackId{1}}},
                delta::InsertRange{.start = 2, .trackIds = {TrackId{1}}}},
    });
    moveTailToFront.applyOrderEditScript(delta::RegularTrackEditScript{
      .edits = {delta::RemoveRange{.start = 1, .trackIds = {TrackId{2}, TrackId{3}}},
                delta::InsertRange{.start = 0, .trackIds = {TrackId{2}, TrackId{3}}}},
    });

    auto const expected = std::vector{TrackId{2}, TrackId{3}, TrackId{1}};
    CHECK(orderTrackIdsOf(moveFirstToEnd) == expected);
    CHECK(sourceTrackIds(moveFirstToEnd) == expected);
    CHECK(orderTrackIdsOf(moveTailToFront) == expected);
    CHECK(sourceTrackIds(moveTailToFront) == expected);

    REQUIRE(firstMoveBatches.size() == 1);
    REQUIRE(sourceEditScript(firstMoveBatches.front()).edits.size() == 2);
    REQUIRE(std::holds_alternative<delta::RemoveRange>(sourceEditScript(firstMoveBatches.front()).edits[0]));
    REQUIRE(std::holds_alternative<delta::InsertRange>(sourceEditScript(firstMoveBatches.front()).edits[1]));
    auto const& firstRemoval = std::get<delta::RemoveRange>(sourceEditScript(firstMoveBatches.front()).edits[0]);
    auto const& firstInsertion = std::get<delta::InsertRange>(sourceEditScript(firstMoveBatches.front()).edits[1]);
    CHECK(firstRemoval.start == 0);
    CHECK(firstRemoval.trackIds == std::vector{TrackId{1}});
    CHECK(firstInsertion.start == 2);
    CHECK(firstInsertion.trackIds == std::vector{TrackId{1}});

    REQUIRE(tailMoveBatches.size() == 1);
    REQUIRE(sourceEditScript(tailMoveBatches.front()).edits.size() == 2);
    REQUIRE(std::holds_alternative<delta::RemoveRange>(sourceEditScript(tailMoveBatches.front()).edits[0]));
    REQUIRE(std::holds_alternative<delta::InsertRange>(sourceEditScript(tailMoveBatches.front()).edits[1]));
    auto const& tailRemoval = std::get<delta::RemoveRange>(sourceEditScript(tailMoveBatches.front()).edits[0]);
    auto const& tailInsertion = std::get<delta::InsertRange>(sourceEditScript(tailMoveBatches.front()).edits[1]);
    CHECK(tailRemoval.start == 1);
    CHECK(tailRemoval.trackIds == std::vector{TrackId{2}, TrackId{3}});
    CHECK(tailInsertion.start == 0);
    CHECK(tailInsertion.trackIds == std::vector{TrackId{2}, TrackId{3}});
  }

  TEST_CASE("ListOrderSource - hidden-only move changes stored order without a source batch",
            "[runtime][unit][source][list-order]")
  {
    auto parentPtr = makeMutableTrackSource({TrackId{1}});
    auto view = ListViewOwner{{TrackId{1}, TrackId{2}, TrackId{3}}};
    auto source = ListOrderSource{view.view().orderTrackIds(), TrackSourceLease{parentPtr}};
    auto batches = std::vector<TrackSourceDelta>{};
    [[maybe_unused]] auto subscription =
      source.subscribe([&batches](TrackSourceDelta const& batch) noexcept { batches.push_back(batch); });

    source.applyOrderEditScript(delta::RegularTrackEditScript{
      .edits = {delta::RemoveRange{.start = 2, .trackIds = {TrackId{3}}},
                delta::InsertRange{.start = 0, .trackIds = {TrackId{3}}}},
    });

    auto const expectedStored = std::vector{TrackId{3}, TrackId{1}, TrackId{2}};
    auto const expectedEffective = std::vector{TrackId{1}};
    CHECK(orderTrackIdsOf(source) == expectedStored);
    CHECK(sourceTrackIds(source) == expectedEffective);
    CHECK(batches.empty());
  }
} // namespace ao::rt::test
