// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/unit/RuntimeTestSupport.h"
#include <ao/library/MusicLibrary.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryWriter.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <vector>

namespace ao::rt::test
{
  TEST_CASE("LibraryChanges - holdback publishes committed changesets in revision order",
            "[runtime][unit][library][changeset]")
  {
    auto executor = ManualExecutor{};
    auto changes = LibraryChanges{executor, 10};
    auto revisions = std::vector<std::uint64_t>{};
    auto subscription = changes.onChanged([&revisions](LibraryChangeSet const& changeSet)
                                          { revisions.push_back(changeSet.libraryRevision); });

    changes.publish(LibraryChangeSet{.libraryRevision = 12, .tracksMutated = {TrackId{12}}});
    changes.publish(LibraryChangeSet{.libraryRevision = 11, .tracksMutated = {TrackId{11}}});

    CHECK(revisions.empty());
    CHECK(executor.queuedCount() == 1);
    executor.runUntilIdle();
    CHECK(revisions == std::vector<std::uint64_t>{11, 12});
  }

  TEST_CASE("LibraryChanges - writer commits publish once with the in-band revision",
            "[runtime][unit][library][changeset]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const trackId = libraryFixture.addTrack("Before");
    auto changes = LibraryChanges{};
    auto writer = LibraryWriter{libraryFixture.library(), changes};
    auto observed = std::vector<LibraryChangeSet>{};
    auto subscription =
      changes.onChanged([&observed](LibraryChangeSet const& changeSet) { observed.push_back(changeSet); });

    REQUIRE(writer.updateMetadata(std::array{trackId}, MetadataPatch{.optTitle = "After"}));
    REQUIRE(writer.updateMetadata(std::array{trackId}, MetadataPatch{.optTitle = "After"}));

    REQUIRE(observed.size() == 2);
    CHECK(observed[0].libraryRevision + 1U == observed[1].libraryRevision);
    CHECK(observed[0].tracksMutated == std::vector{trackId});
    CHECK(observed[1].tracksMutated.empty());
    auto transaction = libraryFixture.library().readTransaction();
    CHECK(libraryFixture.library().libraryRevision(transaction) == observed.back().libraryRevision);
  }

  TEST_CASE("MusicLibrary - aborted write does not advance the snapshot revision", "[library][unit][revision]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    {
      auto transaction = libraryFixture.library().readTransaction();
      CHECK(libraryFixture.library().libraryRevision(transaction) == 0);
    }
    {
      auto transaction = libraryFixture.library().writeTransaction();
      CHECK(libraryFixture.library().libraryRevision(transaction) == 1);
    }
    {
      auto transaction = libraryFixture.library().readTransaction();
      CHECK(libraryFixture.library().libraryRevision(transaction) == 0);
    }
    {
      auto transaction = libraryFixture.library().writeTransaction();
      CHECK(libraryFixture.library().libraryRevision(transaction) == 1);
      REQUIRE(transaction.commit());
    }
    auto transaction = libraryFixture.library().readTransaction();
    CHECK(libraryFixture.library().libraryRevision(transaction) == 1);
  }
} // namespace ao::rt::test
