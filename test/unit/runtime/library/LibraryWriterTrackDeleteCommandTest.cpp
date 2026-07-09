// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/RuntimeTestSupport.h"
#include "test/unit/TestUtils.h"
#include <ao/CoreIds.h>
#include <ao/library/FileManifestBuilder.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/ListBuilder.h>
#include <ao/library/ListStore.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryWriter.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <vector>

namespace ao::rt::test
{
  TEST_CASE("LibraryWriter - deleteTrack removes an existing track and publishes a mutation",
            "[runtime][unit][library][track-delete]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const trackId = libraryFixture.addTrack("Test Track");

    auto changes = LibraryChanges{};
    auto writer = LibraryWriter{libraryFixture.library(), changes};

    auto mutated = std::vector<TrackId>{};
    auto sub = changes.onTracksMutated([&](auto const& trackIds) { mutated = trackIds; });
    auto deletedTracks = std::vector<TrackId>{};
    auto collectionSub = changes.onTrackCollectionChanged([&](auto const& ev) { deletedTracks = ev.deleted; });
    auto upsertedLists = std::vector<ListId>{};
    auto listSub = changes.onListsMutated([&](auto const& ev) { upsertedLists = ev.upserted; });

    auto listIds = std::vector<ListId>{};
    {
      auto transaction = libraryFixture.library().writeTransaction();
      auto manifest = library::FileManifestBuilder::makeEmpty().trackId(trackId).fileSize(10).mtime(20).serialize();
      CHECK(libraryFixture.library().manifest().writer(transaction).put("/tmp/test.flac", manifest));

      for (auto const* const name : std::array{"Manual A", "Manual B"})
      {
        auto listBuilder = library::ListBuilder::makeEmpty();
        listBuilder.name(name).tracks().add(trackId);
        listIds.push_back(
          ao::test::requireValue(libraryFixture.library().lists().writer(transaction).create(listBuilder.serialize()))
            .first);
      }

      REQUIRE(transaction.commit());
    }

    auto const deleted = writer.deleteTrack(trackId);
    REQUIRE(deleted);
    CHECK(deleted->trackId == trackId);
    REQUIRE(mutated.size() == 1);
    CHECK(mutated[0] == trackId);
    REQUIRE(deletedTracks.size() == 1);
    CHECK(deletedTracks[0] == trackId);

    auto transaction = libraryFixture.library().readTransaction();
    auto const optTrackView =
      libraryFixture.library().tracks().reader(transaction).get(trackId, library::TrackStore::Reader::LoadMode::Hot);
    CHECK_FALSE(optTrackView);
    CHECK_FALSE(libraryFixture.library().manifest().reader(transaction).get("/tmp/test.flac"));

    auto listReader = libraryFixture.library().lists().reader(transaction);

    for (auto const listId : listIds)
    {
      auto const optList = listReader.get(listId);
      REQUIRE(optList);
      CHECK(optList->tracks().empty());
    }

    CHECK(upsertedLists == listIds);
  }

  TEST_CASE("LibraryWriter - deleteTrack rejects missing tracks", "[runtime][unit][library][track-delete]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    [[maybe_unused]] auto const trackId = libraryFixture.addTrack("Test Track");

    auto changes = LibraryChanges{};
    auto writer = LibraryWriter{libraryFixture.library(), changes};

    auto mutated = std::vector<TrackId>{};
    auto sub = changes.onTracksMutated([&](auto const& trackIds) { mutated = trackIds; });

    auto const deleted = writer.deleteTrack(TrackId{99999});
    CHECK_FALSE(deleted);
    CHECK(mutated.empty());
  }
} // namespace ao::rt::test
