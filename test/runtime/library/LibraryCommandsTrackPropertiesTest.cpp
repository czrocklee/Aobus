// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/TestFixtureSupport.h"
#include "test/library/TrackTestSupport.h"
#include "test/runtime/RuntimeLibraryTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/DictionaryStore.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackStore.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryCommands.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::rt::test
{
  TEST_CASE("LibraryCommands - track Properties commits metadata and tags in one revision")
  {
    auto storage = MusicLibraryFixture{};
    auto const trackId = storage.addTrack("Before");
    auto changes = makeStateOnlyLibraryChanges(storage.library());
    auto commandsFixture = LibraryCommandsFixture{storage.library(), changes};
    auto publications = std::vector<LibraryChangeSet>{};
    [[maybe_unused]] auto subscription = changes.onChanged([&publications](LibraryChangeSet const& changeSet) noexcept
                                                           { publications.push_back(changeSet); });
    auto targets = commandsFixture.bind(std::array{trackId});
    auto const revisionBefore = targets.revision();

    auto const res = commandsFixture.runTask(commandsFixture.commands().updatePropertiesAsync(
      std::move(targets),
      TrackPropertiesPatch{.metadata = MetadataPatch{.optTitle = "After"}, .tagsToAdd = {"Favorite"}}));

    REQUIRE(res);
    CHECK(res->status == AuthoringStatus::Applied);
    REQUIRE(res->reply.metadata.changes.size() == 1);
    CHECK(res->reply.metadata.changes[0].trackId == trackId);
    REQUIRE(res->reply.tags.changes.size() == 1);
    CHECK(res->reply.tags.changes[0].trackId == trackId);
    REQUIRE(publications.size() == 1);
    CHECK(publications[0].tracksMutated == std::vector<TrackId>{trackId});
    CHECK(publications[0].libraryRevision == revisionBefore + 1);

    auto transaction = storage.library().readTransaction();
    CHECK(storage.library().libraryRevision(transaction) == revisionBefore + 1);
    auto const optTrack =
      storage.library().tracks().reader(transaction).get(trackId, library::TrackStore::Reader::LoadMode::Hot);
    REQUIRE(optTrack);
    auto const track = ao::test::requireValue(
      library::TrackBuilder::fromCommittedHotView(*optTrack, storage.library().dictionary()));
    CHECK(track.metadata().title() == "After");
    CHECK(std::ranges::contains(track.tags().names(), std::string_view{"Favorite"}));
  }

  TEST_CASE("LibraryCommands - track Properties records an explicit title clear while tags stay unchanged")
  {
    auto storage = MusicLibraryFixture{};
    auto const trackId = storage.addTrack(library::test::TrackSpec{.title = "", .tags = {"Favorite"}});
    auto changes = makeStateOnlyLibraryChanges(storage.library());
    auto commandsFixture = LibraryCommandsFixture{storage.library(), changes};
    auto publications = std::vector<LibraryChangeSet>{};
    [[maybe_unused]] auto subscription = changes.onChanged([&publications](LibraryChangeSet const& changeSet) noexcept
                                                           { publications.push_back(changeSet); });
    auto const targets = commandsFixture.bind(std::array{trackId});
    auto const revisionBefore = targets.revision();
    auto const patch = TrackPropertiesPatch{.metadata = MetadataPatch{.optTitle = ""}, .tagsToAdd = {"Favorite"}};

    auto const res = commandsFixture.runTask(commandsFixture.commands().updatePropertiesAsync(targets, patch));
    REQUIRE(res);
    CHECK(res->status == AuthoringStatus::Applied);
    REQUIRE(res->reply.metadata.changes.size() == 1);
    CHECK(res->reply.metadata.changes[0].trackId == trackId);
    CHECK(res->reply.tags.changes.empty());

    REQUIRE(res->optNextTargets);
    auto const repeatRes =
      commandsFixture.runTask(commandsFixture.commands().updatePropertiesAsync(*res->optNextTargets, patch));
    REQUIRE(repeatRes);
    CHECK(repeatRes->status == AuthoringStatus::NoOp);
    CHECK(repeatRes->reply.metadata.changes.empty());
    CHECK(repeatRes->reply.tags.changes.empty());
    REQUIRE(publications.size() == 1);
    CHECK(publications[0].tracksMutated == std::vector<TrackId>{trackId});
    CHECK(publications[0].libraryRevision == revisionBefore + 1);

    auto const transaction = storage.library().readTransaction();
    CHECK(storage.library().libraryRevision(transaction) == revisionBefore + 1);
    auto const optTrack =
      storage.library().tracks().reader(transaction).get(trackId, library::TrackStore::Reader::LoadMode::Hot);
    REQUIRE(optTrack);
    CHECK(optTrack->metadata().isTitleExplicitlyCleared());
    auto const optTagId = storage.library().dictionary().findId("Favorite");
    REQUIRE(optTagId);
    CHECK(optTrack->tags().has(*optTagId));
  }

  TEST_CASE("LibraryCommands - Properties rolls back a staged title clear if another target fails")
  {
    auto storage = MusicLibraryFixture{};
    auto const first = storage.addTrack("");
    auto spec = library::test::makeEmptyTrackSpec("nearly-full.flac");
    spec.customMetadata.emplace_back("Existing", std::string(40'000, 'a'));
    auto const second = storage.addTrack(spec);
    auto changes = makeStateOnlyLibraryChanges(storage.library());
    auto commandsFixture = LibraryCommandsFixture{storage.library(), changes};
    std::size_t publicationCount = 0;
    [[maybe_unused]] auto subscription =
      changes.onChanged([&publicationCount](LibraryChangeSet const&) noexcept { ++publicationCount; });
    auto const targets = commandsFixture.bind(std::array{first, second});
    auto patch = TrackPropertiesPatch{.metadata = MetadataPatch{.optTitle = ""}, .tagsToAdd = {"Favorite"}};
    // This fits on the first track but exceeds the second track's remaining cold record space.
    patch.metadata.customUpdates.emplace("Additional", std::string(40'000, 'b'));

    auto const res = commandsFixture.runTask(commandsFixture.commands().updatePropertiesAsync(targets, patch));
    REQUIRE_FALSE(res);
    CHECK(res.error().code == Error::Code::ValueTooLarge);
    CHECK(publicationCount == 0);
    CHECK_FALSE(storage.library().dictionary().findId("Favorite"));
    CHECK_FALSE(storage.library().dictionary().findId("Additional"));

    auto const transaction = storage.library().readTransaction();
    CHECK(storage.library().libraryRevision(transaction) == targets.revision());
    auto const reader = storage.library().tracks().reader(transaction);
    auto const optFirst = reader.get(first, library::TrackStore::Reader::LoadMode::Both);
    auto const optSecond = reader.get(second, library::TrackStore::Reader::LoadMode::Both);
    REQUIRE(optFirst);
    REQUIRE(optSecond);
    CHECK(optFirst->metadata().title().empty());
    CHECK(optSecond->metadata().title().empty());
    CHECK_FALSE(optFirst->metadata().isTitleExplicitlyCleared());
    CHECK_FALSE(optSecond->metadata().isTitleExplicitlyCleared());
    CHECK(optFirst->tags().empty());
    CHECK(optSecond->tags().empty());
    CHECK(optFirst->customMetadata().empty());
    CHECK(optSecond->customMetadata().count() == 1);
  }

  TEST_CASE("LibraryCommands - invalid tags reject Properties before metadata mutation")
  {
    auto storage = MusicLibraryFixture{};
    auto const trackId = storage.addTrack("Before");
    auto changes = makeStateOnlyLibraryChanges(storage.library());
    auto commandsFixture = LibraryCommandsFixture{storage.library(), changes};
    std::size_t publicationCount = 0;
    [[maybe_unused]] auto subscription =
      changes.onChanged([&publicationCount](LibraryChangeSet const&) noexcept { ++publicationCount; });
    auto targets = commandsFixture.bind(std::array{trackId});
    auto const revisionBefore = targets.revision();
    auto invalidTag = std::string(1, static_cast<char>(0xff));

    auto const res = commandsFixture.runTask(
      commandsFixture.commands().updatePropertiesAsync(std::move(targets),
                                                       TrackPropertiesPatch{
                                                         .metadata = MetadataPatch{.optTitle = "Must roll back"},
                                                         .tagsToAdd = {std::move(invalidTag)},
                                                       }));

    REQUIRE_FALSE(res);
    CHECK(res.error().code == Error::Code::InvalidInput);
    CHECK(publicationCount == 0);

    auto transaction = storage.library().readTransaction();
    CHECK(storage.library().libraryRevision(transaction) == revisionBefore);
    auto const optTrack =
      storage.library().tracks().reader(transaction).get(trackId, library::TrackStore::Reader::LoadMode::Hot);
    REQUIRE(optTrack);
    CHECK(optTrack->metadata().title() == "Before");
    CHECK(optTrack->tags().empty());
  }

  TEST_CASE("LibraryCommands - conflicting Properties tag edits reject the whole patch")
  {
    auto storage = MusicLibraryFixture{};
    auto const trackId = storage.addTrack("Before");
    auto changes = makeStateOnlyLibraryChanges(storage.library());
    auto commandsFixture = LibraryCommandsFixture{storage.library(), changes};
    std::size_t publicationCount = 0;
    [[maybe_unused]] auto subscription =
      changes.onChanged([&publicationCount](LibraryChangeSet const&) noexcept { ++publicationCount; });
    auto targets = commandsFixture.bind(std::array{trackId});

    auto const revision = targets.revision();
    auto const res = commandsFixture.runTask(
      commandsFixture.commands().updatePropertiesAsync(std::move(targets),
                                                       TrackPropertiesPatch{
                                                         .metadata = MetadataPatch{.optTitle = "Must not commit"},
                                                         .tagsToAdd = {"résumé"},
                                                         .tagsToRemove = {"re\u0301sume\u0301"},
                                                       }));

    REQUIRE_FALSE(res);
    CHECK(res.error().code == Error::Code::InvalidInput);
    CHECK(publicationCount == 0);
    CHECK(commandsFixture.bind(std::array{trackId}).revision() == revision);
    auto transaction = storage.library().readTransaction();
    auto const optTrack =
      storage.library().tracks().reader(transaction).get(trackId, library::TrackStore::Reader::LoadMode::Hot);
    REQUIRE(optTrack);
    CHECK(optTrack->metadata().title() == "Before");
    CHECK(optTrack->tags().empty());
  }
} // namespace ao::rt::test
