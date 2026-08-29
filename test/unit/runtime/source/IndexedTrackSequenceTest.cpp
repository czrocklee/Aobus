// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "runtime/source/IndexedTrackSequence.h"

#include "runtime/RuntimeOperationProbe.h"
#include <ao/rt/TrackEditScript.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <vector>

namespace ao::rt::test
{
  TEST_CASE("IndexedTrackSequence - one move updates its index in place",
            "[runtime][unit][source][indexed-track-sequence]")
  {
    auto sequence = IndexedTrackSequence{std::array{TrackId{1}, TrackId{2}, TrackId{3}, TrackId{4}}};
    auto const before = ::ao::rt::detail::RuntimeOperationProbe::counts(sequence);
    auto script = delta::RegularTrackEditScript{
      .edits =
        {
          delta::RemoveRange{.start = 1, .trackIds = {TrackId{2}}},
          delta::InsertRange{.start = 2, .trackIds = {TrackId{2}}},
          delta::UpdateRange{.start = 1, .trackIds = {TrackId{3}}},
        },
    };

    sequence.applyScript(script);

    CHECK(sequence.vector() == std::vector{TrackId{1}, TrackId{3}, TrackId{2}, TrackId{4}});
    CHECK(sequence.indexOf(TrackId{1}) == 0);
    CHECK(sequence.indexOf(TrackId{3}) == 1);
    CHECK(sequence.indexOf(TrackId{2}) == 2);
    CHECK(sequence.indexOf(TrackId{4}) == 3);
    auto const after = ::ao::rt::detail::RuntimeOperationProbe::counts(sequence);
    CHECK(after.indexRebuilds == before.indexRebuilds);
    CHECK(after.incrementalScriptApplications == before.incrementalScriptApplications + 1);
  }

  TEST_CASE("IndexedTrackSequence - multi-range batch rebuilds its index once",
            "[runtime][unit][source][indexed-track-sequence]")
  {
    auto sequence = IndexedTrackSequence{std::array{TrackId{1}, TrackId{2}, TrackId{3}, TrackId{4}}};
    auto const before = ::ao::rt::detail::RuntimeOperationProbe::counts(sequence);
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
    auto const after = ::ao::rt::detail::RuntimeOperationProbe::counts(sequence);
    CHECK(after.indexRebuilds == before.indexRebuilds + 1);
    CHECK(after.incrementalScriptApplications == before.incrementalScriptApplications);
  }
} // namespace ao::rt::test
