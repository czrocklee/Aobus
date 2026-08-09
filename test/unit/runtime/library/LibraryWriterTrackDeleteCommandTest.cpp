// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/TestFixtureSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/library/WritableLibraryTestSupport.h"
#include "test/unit/runtime/RuntimeLibraryTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/FileManifestBuilder.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/LibraryWrite.h>
#include <ao/library/ListBuilder.h>
#include <ao/library/ListStore.h>
#include <ao/library/TrackStore.h>
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
    auto const trackId = libraryFixture.addTrack(library::test::TrackSpec{.title = "Test Track", .uri = "test.flac"});

    auto mutated = std::vector<TrackId>{};
    auto deletedTracks = std::vector<TrackId>{};
    auto upsertedLists = std::vector<ListId>{};

    auto listIds = std::vector<ListId>{};
    {
      auto transaction = library::test::writeTransaction(libraryFixture.library());
      REQUIRE(transaction.apply(
        [&](library::LibraryWrite& write) -> Result<>
        {
          auto manifest = library::FileManifestBuilder::makeEmpty().fileSize(10).mtime(20);

          if (auto updateRes = write.tracks().updateManifest(trackId, manifest); !updateRes)
          {
            return updateRes;
          }

          auto listWriter = write.lists();

          for (auto const* const name : std::array{"Ordered A", "Ordered B"})
          {
            auto listBuilder = library::ListBuilder::makeEmpty();
            listBuilder.name(name).orderTrackIds().add(trackId);
            listIds.push_back(ao::test::requireValue(listWriter.create(listBuilder)));
          }

          return {};
        }));

      REQUIRE(transaction.commit());
    }

    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto sub = changes.onChanged([&](LibraryChangeSet const& event) noexcept { mutated = event.tracksMutated; });
    auto collectionSub =
      changes.onChanged([&](LibraryChangeSet const& ev) noexcept { deletedTracks = ev.tracksDeleted; });
    auto listSub = changes.onChanged([&](LibraryChangeSet const& ev) noexcept { upsertedLists = ev.listsUpserted; });
    auto writerFixture = LibraryWriterFixture{libraryFixture.library(), changes};
    auto& writer = writerFixture.writer();
    auto const deletedRes = writer.deleteTrack(trackId);
    REQUIRE(deletedRes);
    CHECK(deletedRes->trackId == trackId);
    CHECK(mutated.empty());
    REQUIRE(deletedTracks.size() == 1);
    CHECK(deletedTracks[0] == trackId);

    auto transaction = libraryFixture.library().readTransaction();
    auto const optTrackView =
      libraryFixture.library().tracks().reader(transaction).get(trackId, library::TrackStore::Reader::LoadMode::Hot);
    CHECK_FALSE(optTrackView);
    CHECK_FALSE(libraryFixture.library().manifest().reader(transaction).get("test.flac"));

    auto listReader = libraryFixture.library().lists().reader(transaction);

    for (auto const listId : listIds)
    {
      auto const optList = listReader.get(listId);
      REQUIRE(optList);
      CHECK(optList->orderTrackIds().empty());
    }

    CHECK(upsertedLists == listIds);
  }

  TEST_CASE("LibraryWriter - deleteTrack rejects missing tracks", "[runtime][unit][library][track-delete]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    [[maybe_unused]] auto const trackId = libraryFixture.addTrack("Test Track");

    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto writerFixture = LibraryWriterFixture{libraryFixture.library(), changes};
    auto& writer = writerFixture.writer();

    auto mutated = std::vector<TrackId>{};
    auto sub = changes.onChanged([&](LibraryChangeSet const& event) noexcept { mutated = event.tracksMutated; });

    auto const deletedRes = writer.deleteTrack(TrackId{99999});
    CHECK_FALSE(deletedRes);
    CHECK(mutated.empty());
  }
} // namespace ao::rt::test
