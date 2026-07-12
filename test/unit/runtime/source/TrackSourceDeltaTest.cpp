// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/CoreIds.h>
#include <ao/rt/source/TrackSourceDelta.h>

#include <catch2/catch_test_macros.hpp>

namespace ao::rt::test
{
  TEST_CASE("TrackSourceDelta - mixed ranges use sequential coordinates", "[runtime][unit][source]")
  {
    auto const batch = TrackSourceDeltaBatch{
      .revision = 4,
      .deltas =
        {
          SourceRemoveRange{.start = 1, .trackIds = {TrackId{20}}},
          SourceInsertRange{.start = 2, .trackIds = {TrackId{40}, TrackId{50}}},
          SourceUpdateRange{.start = 3, .trackIds = {TrackId{50}}},
        },
    };

    CHECK(validateTrackSourceDeltaBatch(batch, 3));
  }

  TEST_CASE("TrackSourceDelta - a later range is validated in the preceding delta coordinate space",
            "[runtime][unit][source]")
  {
    auto const batch = TrackSourceDeltaBatch{
      .revision = 2,
      .deltas =
        {
          SourceRemoveRange{.start = 1, .trackIds = {TrackId{20}, TrackId{30}}},
          SourceUpdateRange{.start = 2, .trackIds = {TrackId{40}}},
        },
    };

    CHECK_FALSE(validateTrackSourceDeltaBatch(batch, 4));
  }

  TEST_CASE("TrackSourceDelta - reset and invalidation require singleton batches", "[runtime][unit][source]")
  {
    auto const reset = TrackSourceDeltaBatch{.revision = 2, .deltas = {SourceReset{}}};
    auto const invalidated = TrackSourceDeltaBatch{.revision = 3, .deltas = {SourceInvalidated{}}};
    auto const resetWithRange = TrackSourceDeltaBatch{
      .revision = 4,
      .deltas = {SourceReset{}, SourceInsertRange{.start = 0, .trackIds = {TrackId{10}}}},
    };
    auto const rangeWithInvalidation = TrackSourceDeltaBatch{
      .revision = 5,
      .deltas = {SourceRemoveRange{.start = 0, .trackIds = {TrackId{10}}}, SourceInvalidated{}},
    };

    CHECK(validateTrackSourceDeltaBatch(reset, 7));
    CHECK(validateTrackSourceDeltaBatch(invalidated, 7));
    CHECK_FALSE(validateTrackSourceDeltaBatch(resetWithRange, 0));
    CHECK_FALSE(validateTrackSourceDeltaBatch(rangeWithInvalidation, 1));
  }

  TEST_CASE("TrackSourceDelta - empty batches and empty ranges are invalid", "[runtime][unit][source]")
  {
    auto const emptyBatch = TrackSourceDeltaBatch{.revision = 1};
    auto const emptyInsert = TrackSourceDeltaBatch{.revision = 2, .deltas = {SourceInsertRange{.start = 0}}};
    auto const emptyRemove = TrackSourceDeltaBatch{.revision = 3, .deltas = {SourceRemoveRange{.start = 0}}};
    auto const emptyUpdate = TrackSourceDeltaBatch{.revision = 4, .deltas = {SourceUpdateRange{.start = 0}}};

    CHECK_FALSE(validateTrackSourceDeltaBatch(emptyBatch, 0));
    CHECK_FALSE(validateTrackSourceDeltaBatch(emptyInsert, 0));
    CHECK_FALSE(validateTrackSourceDeltaBatch(emptyRemove, 0));
    CHECK_FALSE(validateTrackSourceDeltaBatch(emptyUpdate, 0));
  }
} // namespace ao::rt::test
