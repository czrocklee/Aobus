// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/rt/source/TrackSourceDelta.h>

#include <ao/CoreIds.h>
#include <ao/rt/TrackEditScript.h>

#include <catch2/catch_test_macros.hpp>

namespace ao::rt::test
{
  TEST_CASE("TrackSourceDelta - a regular script uses sequential coordinates", "[runtime][unit][source]")
  {
    auto const message = TrackSourceDelta{delta::RegularTrackEditScript{
      .edits = {delta::RemoveRange{.start = 1, .trackIds = {TrackId{20}}},
                delta::InsertRange{.start = 2, .trackIds = {TrackId{40}, TrackId{50}}},
                delta::UpdateRange{.start = 3, .trackIds = {TrackId{50}}}},
    }};

    CHECK(validateTrackSourceDelta(message, 3));
  }

  TEST_CASE("TrackSourceDelta - a later range is validated in the preceding delta coordinate space",
            "[runtime][unit][source]")
  {
    auto const message = TrackSourceDelta{delta::RegularTrackEditScript{
      .edits = {delta::RemoveRange{.start = 1, .trackIds = {TrackId{20}, TrackId{30}}},
                delta::UpdateRange{.start = 2, .trackIds = {TrackId{40}}}},
    }};

    CHECK_FALSE(validateTrackSourceDelta(message, 4));
  }

  TEST_CASE("TrackSourceDelta - reset and invalidation are standalone alternatives", "[runtime][unit][source]")
  {
    auto const reset = TrackSourceDelta{SourceReset{}};
    auto const invalidated = TrackSourceDelta{SourceInvalidated{}};

    CHECK(validateTrackSourceDelta(reset, 7));
    CHECK(validateTrackSourceDelta(invalidated, 7));
  }

  TEST_CASE("TrackSourceDelta - empty scripts and empty ranges are invalid", "[runtime][unit][source]")
  {
    auto const emptyScript = TrackSourceDelta{delta::RegularTrackEditScript{}};
    auto const emptyInsert = TrackSourceDelta{delta::RegularTrackEditScript{.edits = {delta::InsertRange{.start = 0}}}};
    auto const emptyRemove = TrackSourceDelta{delta::RegularTrackEditScript{.edits = {delta::RemoveRange{.start = 0}}}};
    auto const emptyUpdate = TrackSourceDelta{delta::RegularTrackEditScript{.edits = {delta::UpdateRange{.start = 0}}}};

    CHECK_FALSE(validateTrackSourceDelta(emptyScript, 0));
    CHECK_FALSE(validateTrackSourceDelta(emptyInsert, 0));
    CHECK_FALSE(validateTrackSourceDelta(emptyRemove, 0));
    CHECK_FALSE(validateTrackSourceDelta(emptyUpdate, 0));
  }
} // namespace ao::rt::test
