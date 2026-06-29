// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/RuntimeTestUtils.h"
#include <ao/CoreIds.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryWriter.h>

#include <catch2/catch_test_macros.hpp>

#include <vector>

namespace ao::rt::test
{
  TEST_CASE("LibraryWriter deleteTrack removes an existing track and publishes a mutation",
            "[runtime][unit][library][mutation][track][delete]")
  {
    auto testLib = TestMusicLibrary{};
    auto const trackId = testLib.addTrack("Test Track");

    auto changes = LibraryChanges{};
    auto writer = LibraryWriter{testLib.library(), changes};

    auto mutated = std::vector<TrackId>{};
    auto sub = changes.onTracksMutated([&](auto const& trackIds) { mutated = trackIds; });

    bool const deleted = writer.deleteTrack(trackId);
    CHECK(deleted);
    REQUIRE(mutated.size() == 1);
    CHECK(mutated[0] == trackId);

    auto txn = testLib.library().readTransaction();
    auto const optTrackView =
      testLib.library().tracks().reader(txn).get(trackId, library::TrackStore::Reader::LoadMode::Hot);
    CHECK_FALSE(optTrackView.has_value());
  }

  TEST_CASE("LibraryWriter deleteTrack rejects missing tracks", "[runtime][unit][library][mutation][track][delete]")
  {
    auto testLib = TestMusicLibrary{};
    [[maybe_unused]] auto const trackId = testLib.addTrack("Test Track");

    auto changes = LibraryChanges{};
    auto writer = LibraryWriter{testLib.library(), changes};

    auto mutated = std::vector<TrackId>{};
    auto sub = changes.onTracksMutated([&](auto const& trackIds) { mutated = trackIds; });

    bool const deleted = writer.deleteTrack(TrackId{99999});
    CHECK_FALSE(deleted);
    CHECK(mutated.empty());
  }
} // namespace ao::rt::test
