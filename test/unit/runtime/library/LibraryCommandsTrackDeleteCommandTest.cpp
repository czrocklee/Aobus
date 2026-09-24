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
#include <ao/rt/TrackEditScript.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryCommands.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <vector>

namespace ao::rt::test
{
  TEST_CASE("LibraryCommands - deleteTrack removes an existing track and publishes a mutation",
            "[runtime][unit][library][track-delete]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const trackId = libraryFixture.addTrack(library::test::TrackSpec{.title = "Test Track", .uri = "test.flac"});
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
    REQUIRE(listIds.size() == 2);

    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto events = std::vector<LibraryChangeSet>{};
    auto sub = changes.onChanged([&events](LibraryChangeSet const& event) noexcept { events.push_back(event); });
    auto commandsFixture = LibraryCommandsFixture{libraryFixture.library(), changes};
    auto& commands = commandsFixture.commands();
    auto const revisionBefore = libraryFixture.library().libraryRevision(libraryFixture.library().readTransaction());

    auto const deletedRes = commandsFixture.runTask(commands.deleteTrackAsync(trackId));

    REQUIRE(deletedRes);
    CHECK(deletedRes->trackId == trackId);
    CHECK(deletedRes->uri == "test.flac");
    CHECK(deletedRes->title == "Test Track");
    CHECK(deletedRes->removedFromListIds == listIds);
    REQUIRE(events.size() == 1);
    CHECK(events.front() == LibraryChangeSet{
                              .libraryRevision = revisionBefore + 1,
                              .tracksDeleted = {trackId},
                              .listsUpserted = listIds,
                              .listOrderChanges =
                                {
                                  ListOrderChange{
                                    .listId = listIds[0],
                                    .operation =
                                      delta::RegularTrackEditScript{
                                        .edits = {delta::RemoveRange{.start = 0, .trackIds = {trackId}}},
                                      },
                                  },
                                  ListOrderChange{
                                    .listId = listIds[1],
                                    .operation =
                                      delta::RegularTrackEditScript{
                                        .edits = {delta::RemoveRange{.start = 0, .trackIds = {trackId}}},
                                      },
                                  },
                                },
                            });

    auto transaction = libraryFixture.library().readTransaction();
    CHECK(libraryFixture.library().libraryRevision(transaction) == revisionBefore + 1);
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
  }

  TEST_CASE("LibraryCommands - deleteTrack rejects missing tracks", "[runtime][unit][library][track-delete]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const trackId = libraryFixture.addTrack("Test Track");
    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto commandsFixture = LibraryCommandsFixture{libraryFixture.library(), changes};
    auto& commands = commandsFixture.commands();
    auto const revisionBefore = libraryFixture.library().libraryRevision(libraryFixture.library().readTransaction());
    auto events = std::vector<LibraryChangeSet>{};
    auto sub = changes.onChanged([&events](LibraryChangeSet const& event) noexcept { events.push_back(event); });

    auto const deletedRes = commandsFixture.runTask(commands.deleteTrackAsync(TrackId{99999}));

    REQUIRE_FALSE(deletedRes);
    CHECK(deletedRes.error().code == Error::Code::NotFound);
    CHECK(events.empty());
    auto transaction = libraryFixture.library().readTransaction();
    CHECK(libraryFixture.library().libraryRevision(transaction) == revisionBefore);
    auto const reader = libraryFixture.library().tracks().reader(transaction);
    CHECK(reader.entryCount() == 1);
    auto const optTrack = reader.get(trackId, library::TrackStore::Reader::LoadMode::Both);
    REQUIRE(optTrack);
    CHECK(optTrack->metadata().title() == "Test Track");
    CHECK(optTrack->property().uri() == "test.flac");
    CHECK(libraryFixture.library().manifest().reader(transaction).get("test.flac"));
  }
} // namespace ao::rt::test
