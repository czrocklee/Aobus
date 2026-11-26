// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "runtime/library/LibraryCommandsInternal.h"
#include "test/TestFixtureSupport.h"
#include "test/library/TrackTestSupport.h"
#include "test/library/WritableLibraryTestSupport.h"
#include "test/runtime/RuntimeLibraryTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/DictionaryStore.h>
#include <ao/library/LibraryWrite.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackStore.h>
#include <ao/library/TrackWriter.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryCommands.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    std::vector<std::string> committedTagNames(library::MusicLibrary const& storage, TrackId const id)
    {
      auto const transaction = storage.readTransaction();
      auto const optView = storage.tracks().reader(transaction).get(id, library::TrackStore::Reader::LoadMode::Hot);
      REQUIRE(optView);
      auto const builder =
        ao::test::requireValue(library::TrackBuilder::fromCommittedHotView(*optView, storage.dictionary()));
      auto const& names = builder.tags().names();
      return {names.begin(), names.end()};
    }
  } // namespace

  TEST_CASE("LibraryCommands - editTags adds a new tag and publishes a mutation")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const trackId = libraryFixture.addTrack("Test Track");

    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto commandsFixture = LibraryCommandsFixture{libraryFixture.library(), changes};
    auto& writer = commandsFixture;

    auto mutated = std::vector<TrackId>{};
    auto sub =
      changes.onChanged([&](LibraryChangeSet const& changeSet) noexcept { mutated = changeSet.tracksMutated; });

    auto const favorite = std::array{std::string{"Favorite"}};

    auto const replyRes = writer.editTags(std::array{trackId}, favorite, {});
    REQUIRE(replyRes);
    CHECK_FALSE(replyRes->changes.empty());
    REQUIRE(mutated.size() == 1);
    CHECK(mutated[0] == trackId);

    CHECK(committedTagNames(libraryFixture.library(), trackId) == std::vector<std::string>{"Favorite"});
  }

  TEST_CASE("LibraryCommands - a tag patch sees edits staged earlier in the same transaction")
  {
    auto fixture = MusicLibraryFixture{};
    auto& storage = fixture.library();
    auto const targetId = fixture.addTrack(library::test::TrackSpec{.title = "Target", .tags = {"Original"}});
    auto const otherId = fixture.addTrack("Other track");
    auto const added = std::array{std::string{"Final"}};
    auto const removed = std::array{std::string{"Staged"}};
    auto const& dictionary = storage.dictionary();
    auto transaction = library::test::writeTransaction(storage);

    auto const replyRes = transaction.apply(
      [&](library::LibraryWrite& write)
      {
        auto writer = write.tracks();
        auto stageTag = [&writer](TrackId const id, std::string_view const tag)
        {
          auto optBuilder = writer.loadForEdit(id, library::TrackWriter::EditTier::Hot);
          REQUIRE(optBuilder);
          optBuilder->tags().add(tag);
          REQUIRE(writer.updateHot(id, *optBuilder));
        };
        stageTag(targetId, removed.front());
        stageTag(otherId, "Unrelated");
        return ao::rt::detail::applyTagPatchInTransaction(write, std::array{targetId}, added, removed);
      });

    REQUIRE(replyRes);
    REQUIRE(replyRes->changes.size() == 1);
    auto const& change = replyRes->changes.front();
    CHECK(change.trackId == targetId);
    CHECK(change.addedTags == std::vector<std::string>{"Final"});
    CHECK(change.removedTags == std::vector<std::string>{"Staged"});
    CHECK_FALSE(dictionary.findId("Staged"));
    CHECK_FALSE(dictionary.findId("Unrelated"));
    CHECK_FALSE(dictionary.findId("Final"));
    CHECK(committedTagNames(storage, targetId) == std::vector<std::string>{"Original"});
    CHECK(committedTagNames(storage, otherId).empty());

    REQUIRE(transaction.commit());
    CHECK(committedTagNames(storage, targetId) == std::vector<std::string>{"Original", "Final"});
    CHECK(committedTagNames(storage, otherId) == std::vector<std::string>{"Unrelated"});
  }

  TEST_CASE("LibraryCommands - editTags ignores an existing tag")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const trackId = libraryFixture.addTrack("Test Track");

    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto commandsFixture = LibraryCommandsFixture{libraryFixture.library(), changes};
    auto& writer = commandsFixture;

    auto mutated = std::vector<TrackId>{};
    auto sub =
      changes.onChanged([&](LibraryChangeSet const& changeSet) noexcept { mutated = changeSet.tracksMutated; });

    auto const favorite = std::array{std::string{"Favorite"}};

    REQUIRE(writer.editTags(std::array{trackId}, favorite, {}));
    mutated.clear();

    auto const replyRes = writer.editTags(std::array{trackId}, favorite, {});
    REQUIRE(replyRes);
    CHECK(replyRes->changes.empty());
    CHECK(mutated.empty());
  }

  TEST_CASE("LibraryCommands - editTags uses canonical Unicode identity")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const trackId = libraryFixture.addTrack("Test Track");
    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto commandsFixture = LibraryCommandsFixture{libraryFixture.library(), changes};
    auto const composed = std::array{std::string{"résumé"}};
    auto const decomposed = std::array{std::string{"re\u0301sume\u0301"}};

    REQUIRE(commandsFixture.editTags(std::array{trackId}, composed, {}));
    auto const duplicateRes = commandsFixture.editTags(std::array{trackId}, decomposed, {});
    REQUIRE(duplicateRes);
    CHECK(duplicateRes->changes.empty());

    auto const removeRes = commandsFixture.editTags(std::array{trackId}, {}, decomposed);
    REQUIRE(removeRes);
    REQUIRE(removeRes->changes.size() == 1);
    REQUIRE(removeRes->changes[0].removedTags.size() == 1);
    CHECK(removeRes->changes[0].removedTags[0] == "résumé");
  }

  TEST_CASE("LibraryCommands - conflicting tag edits preserve membership and revision")
  {
    auto storage = MusicLibraryFixture{};
    auto const trackId = storage.addTrack("Track");
    auto changes = makeStateOnlyLibraryChanges(storage.library());
    auto commandsFixture = LibraryCommandsFixture{storage.library(), changes};
    auto const tag = std::array{std::string{"Favorite"}};
    bool initiallyPresent = false;

    SECTION("The tag starts absent")
    {
    }

    SECTION("The tag starts present")
    {
      REQUIRE(commandsFixture.editTags(std::array{trackId}, tag, {}));
      initiallyPresent = true;
    }

    auto const revision = commandsFixture.bind(std::array{trackId}).revision();
    std::size_t publicationCount = 0;
    [[maybe_unused]] auto subscription =
      changes.onChanged([&publicationCount](LibraryChangeSet const&) noexcept { ++publicationCount; });

    auto const res = commandsFixture.editTags(std::array{trackId}, tag, tag);

    REQUIRE_FALSE(res);
    CHECK(res.error().code == Error::Code::InvalidInput);
    CHECK(publicationCount == 0);
    CHECK(commandsFixture.bind(std::array{trackId}).revision() == revision);
    CHECK(std::ranges::contains(committedTagNames(storage.library(), trackId), tag.front()) == initiallyPresent);
  }

  TEST_CASE("LibraryCommands - editTags rejects missing tag-add targets")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const trackId = libraryFixture.addTrack("Test Track");

    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto commandsFixture = LibraryCommandsFixture{libraryFixture.library(), changes};
    auto& writer = commandsFixture;
    auto const revisionBefore = writer.bind(std::array{trackId}).revision();

    std::size_t publicationCount = 0;
    auto sub = changes.onChanged([&publicationCount](LibraryChangeSet const&) noexcept { ++publicationCount; });

    auto const favorite = std::array{std::string{"Favorite"}};

    auto const replyRes = writer.editTags(std::array{TrackId{99999}}, favorite, {});
    REQUIRE_FALSE(replyRes);
    CHECK(replyRes.error().code == Error::Code::NotFound);
    CHECK(publicationCount == 0);
    auto transaction = libraryFixture.library().readTransaction();
    CHECK(libraryFixture.library().libraryRevision(transaction) == revisionBefore);
  }

  TEST_CASE("LibraryCommands - editTags removes an existing tag and publishes a mutation")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const trackId = libraryFixture.addTrack("Test Track");

    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto commandsFixture = LibraryCommandsFixture{libraryFixture.library(), changes};
    auto& writer = commandsFixture;

    auto mutated = std::vector<TrackId>{};
    auto sub =
      changes.onChanged([&](LibraryChangeSet const& changeSet) noexcept { mutated = changeSet.tracksMutated; });

    auto const favorite = std::array{std::string{"Favorite"}};

    REQUIRE(writer.editTags(std::array{trackId}, favorite, {}));
    mutated.clear();

    auto const replyRes = writer.editTags(std::array{trackId}, {}, favorite);
    REQUIRE(replyRes);
    CHECK_FALSE(replyRes->changes.empty());
    REQUIRE(mutated.size() == 1);
    CHECK(mutated[0] == trackId);

    auto transaction = libraryFixture.library().readTransaction();
    auto const optTrack =
      libraryFixture.library().tracks().reader(transaction).get(trackId, library::TrackStore::Reader::LoadMode::Hot);
    REQUIRE(optTrack);
    CHECK(optTrack->tags().empty());
  }

  TEST_CASE("LibraryCommands - editTags ignores missing tags")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const trackId = libraryFixture.addTrack("Test Track");

    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto commandsFixture = LibraryCommandsFixture{libraryFixture.library(), changes};
    auto& writer = commandsFixture;

    auto mutated = std::vector<TrackId>{};
    auto sub =
      changes.onChanged([&](LibraryChangeSet const& changeSet) noexcept { mutated = changeSet.tracksMutated; });

    auto const nonExistent = std::array{std::string{"NonExistent"}};
    auto const replyRes = writer.editTags(std::array{trackId}, {}, nonExistent);
    REQUIRE(replyRes);
    CHECK(replyRes->changes.empty());
    CHECK(mutated.empty());
  }

  TEST_CASE("LibraryCommands - editTags rejects missing tag-remove targets")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const trackId = libraryFixture.addTrack("Test Track");

    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto commandsFixture = LibraryCommandsFixture{libraryFixture.library(), changes};
    auto& writer = commandsFixture;
    auto const revisionBefore = writer.bind(std::array{trackId}).revision();

    std::size_t publicationCount = 0;
    auto sub = changes.onChanged([&publicationCount](LibraryChangeSet const&) noexcept { ++publicationCount; });

    auto const favorite = std::array{std::string{"Favorite"}};

    auto const replyRes = writer.editTags(std::array{TrackId{99999}}, {}, favorite);
    REQUIRE_FALSE(replyRes);
    CHECK(replyRes.error().code == Error::Code::NotFound);
    CHECK(publicationCount == 0);
    auto transaction = libraryFixture.library().readTransaction();
    CHECK(libraryFixture.library().libraryRevision(transaction) == revisionBefore);
  }
} // namespace ao::rt::test
