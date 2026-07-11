// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "runtime/playback/ProjectionAnchor.h"

#include <ao/CoreIds.h>
#include <ao/Exception.h>
#include <ao/rt/projection/TrackListProjection.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <optional>

namespace ao::rt::test
{
  namespace
  {
    constexpr auto kCurrentTrack = TrackId{42};

    TrackListProjectionDeltaBatch insertBatch(std::size_t const start, std::size_t const count)
    {
      return TrackListProjectionDeltaBatch{
        .deltas = {ProjectionInsertRange{TrackRowRange{.start = start, .count = count}}},
      };
    }

    TrackListProjectionDeltaBatch removeBatch(std::size_t const start, std::size_t const count)
    {
      return TrackListProjectionDeltaBatch{
        .deltas = {ProjectionRemoveRange{TrackRowRange{.start = start, .count = count}}},
      };
    }

    TrackListProjectionDeltaBatch resetBatch()
    {
      return TrackListProjectionDeltaBatch{.deltas = {ProjectionReset{}}};
    }
  } // namespace

  TEST_CASE("ProjectionAnchor - insertion boundaries distinguish Bound from Gap", "[runtime][unit][playback-cursor]")
  {
    auto insertedBeforeGap = ProjectionAnchor::gap(kCurrentTrack, 2, 4);
    insertedBeforeGap.applyBatch(insertBatch(1, 2), 6, std::nullopt);
    CHECK(insertedBeforeGap.state() == ProjectionAnchor::State::Gap);
    CHECK(insertedBeforeGap.anchorIndex() == 4);

    auto insertedAtGap = ProjectionAnchor::gap(kCurrentTrack, 2, 4);
    insertedAtGap.applyBatch(insertBatch(2, 2), 6, std::nullopt);
    CHECK(insertedAtGap.state() == ProjectionAnchor::State::Gap);
    CHECK(insertedAtGap.anchorIndex() == 2);

    auto insertedAtBound = ProjectionAnchor::bound(kCurrentTrack, 2, 4);
    insertedAtBound.applyBatch(insertBatch(2, 2), 6, 4);
    CHECK(insertedAtBound.state() == ProjectionAnchor::State::Bound);
    CHECK(insertedAtBound.anchorIndex() == 4);
  }

  TEST_CASE("ProjectionAnchor - removal boundaries preserve the successor gap", "[runtime][unit][playback-cursor]")
  {
    auto removedBefore = ProjectionAnchor::gap(kCurrentTrack, 4, 6);
    removedBefore.applyBatch(removeBatch(1, 3), 3, std::nullopt);
    CHECK(removedBefore.state() == ProjectionAnchor::State::Gap);
    CHECK(removedBefore.anchorIndex() == 1);

    auto removedAtGap = ProjectionAnchor::gap(kCurrentTrack, 4, 6);
    removedAtGap.applyBatch(removeBatch(4, 1), 5, std::nullopt);
    CHECK(removedAtGap.state() == ProjectionAnchor::State::Gap);
    CHECK(removedAtGap.anchorIndex() == 4);

    auto removedAfter = ProjectionAnchor::gap(kCurrentTrack, 4, 6);
    removedAfter.applyBatch(removeBatch(5, 1), 5, std::nullopt);
    CHECK(removedAfter.state() == ProjectionAnchor::State::Gap);
    CHECK(removedAfter.anchorIndex() == 4);
  }

  TEST_CASE("ProjectionAnchor - removing the current row enters Gap at its old position",
            "[runtime][unit][playback-cursor]")
  {
    auto anchor = ProjectionAnchor::bound(kCurrentTrack, 2, 5);

    anchor.applyBatch(removeBatch(1, 2), 3, std::nullopt);

    CHECK(anchor.trackId() == kCurrentTrack);
    CHECK(anchor.state() == ProjectionAnchor::State::Gap);
    CHECK(anchor.anchorIndex() == 1);
  }

  TEST_CASE("ProjectionAnchor - sequential move of the current row reconciles directly to Bound",
            "[runtime][unit][playback-cursor]")
  {
    auto anchor = ProjectionAnchor::bound(kCurrentTrack, 1, 4);
    auto const moveBatch = TrackListProjectionDeltaBatch{
      .deltas =
        {
          ProjectionRemoveRange{TrackRowRange{.start = 1, .count = 1}},
          ProjectionInsertRange{TrackRowRange{.start = 3, .count = 1}},
        },
    };

    anchor.applyBatch(moveBatch, 4, 3);

    CHECK(anchor.state() == ProjectionAnchor::State::Bound);
    CHECK(anchor.anchorIndex() == 3);
  }

  TEST_CASE("ProjectionAnchor - regular batches transform sequential coordinates before reconciliation",
            "[runtime][unit][playback-cursor]")
  {
    auto anchor = ProjectionAnchor::gap(kCurrentTrack, 4, 6);
    auto const batch = TrackListProjectionDeltaBatch{
      .deltas =
        {
          ProjectionInsertRange{TrackRowRange{.start = 1, .count = 2}},
          ProjectionRemoveRange{TrackRowRange{.start = 2, .count = 3}},
          ProjectionUpdateRange{TrackRowRange{.start = 1, .count = 2}},
          ProjectionInsertRange{TrackRowRange{.start = 3, .count = 1}},
        },
    };

    anchor.applyBatch(batch, 6, std::nullopt);

    CHECK(anchor.state() == ProjectionAnchor::State::Gap);
    CHECK(anchor.anchorIndex() == 3);
  }

  TEST_CASE("ProjectionAnchor - Reset reconciles identity and clamps a missing current",
            "[runtime][unit][playback-cursor]")
  {
    auto missing = ProjectionAnchor::bound(kCurrentTrack, 4, 5);
    missing.applyBatch(resetBatch(), 3, std::nullopt);
    CHECK(missing.state() == ProjectionAnchor::State::Gap);
    CHECK(missing.anchorIndex() == 3);

    auto present = ProjectionAnchor::gap(kCurrentTrack, 4, 5);
    present.applyBatch(resetBatch(), 3, 1);
    CHECK(present.state() == ProjectionAnchor::State::Bound);
    CHECK(present.anchorIndex() == 1);
  }

  TEST_CASE("ProjectionAnchor - source invalidation is rejected as terminal", "[runtime][unit][playback-cursor]")
  {
    auto anchor = ProjectionAnchor::bound(kCurrentTrack, 1, 3);
    auto const terminalBatch = TrackListProjectionDeltaBatch{
      .deltas = {ProjectionSourceInvalidated{}},
    };

    REQUIRE_THROWS_AS(anchor.applyBatch(terminalBatch, 0, std::nullopt), Exception);
  }

  TEST_CASE("ProjectionAnchor - empty and end gaps retain successor semantics", "[runtime][unit][playback-cursor]")
  {
    auto emptied = ProjectionAnchor::bound(kCurrentTrack, 0, 1);
    emptied.applyBatch(resetBatch(), 0, std::nullopt);
    CHECK(emptied.state() == ProjectionAnchor::State::Gap);
    CHECK(emptied.anchorIndex() == 0);

    auto endGap = ProjectionAnchor::gap(kCurrentTrack, 3, 3);
    endGap.applyBatch(insertBatch(3, 2), 5, std::nullopt);
    CHECK(endGap.state() == ProjectionAnchor::State::Gap);
    CHECK(endGap.anchorIndex() == 3);
  }

  TEST_CASE("ProjectionAnchor - factories enforce anchor range invariants", "[runtime][unit][playback-cursor]")
  {
    auto const clamped = ProjectionAnchor::gap(kCurrentTrack, 99, 4);
    CHECK(clamped.state() == ProjectionAnchor::State::Gap);
    CHECK(clamped.anchorIndex() == 4);

    REQUIRE_THROWS_AS(ProjectionAnchor::bound(kCurrentTrack, 4, 4), Exception);
    REQUIRE_THROWS_AS(ProjectionAnchor::gap(kInvalidTrackId, 0, 0), Exception);
  }
} // namespace ao::rt::test
