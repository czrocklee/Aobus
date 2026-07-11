// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/rt/TrackEditScript.h>
#include <ao/rt/source/IndexedTrackSequence.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <vector>

namespace ao::rt::test
{
  TEST_CASE("IndexedTrackSequence - one batch rebuilds its index once",
            "[runtime][unit][source][indexed-track-sequence]")
  {
    auto sequence = IndexedTrackSequence{std::array{TrackId{1}, TrackId{2}, TrackId{3}, TrackId{4}}};
    auto const before = sequence.operationCounts();
    auto script = delta::RegularTrackEditScript{
      .edits =
        {
          delta::RemoveRange{.start = 2, .trackIds = {TrackId{3}}},
          delta::RemoveRange{.start = 0, .trackIds = {TrackId{1}}},
          delta::InsertRange{.start = 1, .trackIds = {TrackId{5}, TrackId{6}}},
          delta::UpdateRange{.start = 0, .trackIds = {TrackId{2}}},
        },
    };

    sequence.applyScript(script);

    CHECK(sequence.vector() == std::vector{TrackId{2}, TrackId{5}, TrackId{6}, TrackId{4}});
    CHECK(sequence.indexOf(TrackId{2}) == 0);
    CHECK(sequence.indexOf(TrackId{6}) == 2);
    CHECK_FALSE(sequence.contains(TrackId{1}));
    CHECK(sequence.operationCounts().indexRebuilds == before.indexRebuilds + 1);
  }
} // namespace ao::rt::test
