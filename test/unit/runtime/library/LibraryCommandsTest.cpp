// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/library/LibraryCommands.h>

#include "test/unit/TestFixtureSupport.h"
#include "test/unit/runtime/RuntimeLibraryTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/DictionaryStore.h>
#include <ao/library/ListStore.h>
#include <ao/library/TrackStore.h>
#include <ao/rt/ListMutation.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibrarySnapshot.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::rt::test
{
  TEST_CASE("LibraryCommands - updateMetadata publishes TracksMutated", "[runtime][unit][library][mutation]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const trackId = libraryFixture.addTrack("Original Title");

    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto commandsFixture = LibraryCommandsFixture{libraryFixture.library(), changes};

    auto mutated = std::vector<TrackId>{};
    auto sub = changes.onChanged([&](LibraryChangeSet const& event) noexcept { mutated = event.tracksMutated; });

    auto const targetIds = std::array{trackId};
    auto const result = commandsFixture.updateMetadata(targetIds, MetadataPatch{.optTitle = "New Title"});

    REQUIRE(result);
    CHECK_FALSE(result->changes.empty());
    REQUIRE(mutated.size() == 1);
    CHECK(mutated[0] == trackId);
  }

  TEST_CASE("LibraryCommands - observers see committed metadata", "[runtime][unit][library][mutation]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const trackId = libraryFixture.addTrack("Original Title");

    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto commandsFixture = LibraryCommandsFixture{libraryFixture.library(), changes};

    std::size_t observedMutationCount = 0;
    auto observedTrackId = TrackId{kInvalidTrackId};
    std::uint64_t observedRevision = 0;
    auto sub = changes.onChanged(
      [&](LibraryChangeSet const& event) noexcept
      {
        observedMutationCount = event.tracksMutated.size();
        observedTrackId = event.tracksMutated.empty() ? kInvalidTrackId : event.tracksMutated.front();
        observedRevision = event.libraryRevision;
      });

    auto const targetIds = std::array{trackId};
    auto const result = commandsFixture.updateMetadata(targetIds, MetadataPatch{.optTitle = "Committed Title"});

    REQUIRE(result);
    CHECK_FALSE(result->changes.empty());
    REQUIRE(observedMutationCount == 1);
    CHECK(observedTrackId == trackId);

    auto transaction = libraryFixture.library().readTransaction();
    CHECK(libraryFixture.library().libraryRevision(transaction) == observedRevision);
    auto const optView =
      libraryFixture.library().tracks().reader(transaction).get(trackId, library::TrackStore::Reader::LoadMode::Hot);
    REQUIRE(optView);
    CHECK(optView->metadata().title() == "Committed Title");
  }

  TEST_CASE("LibraryCommands - updateMetadata reports no mutation for identical values",
            "[runtime][unit][library][mutation]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const trackId = libraryFixture.addTrack("Original Title");

    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto commandsFixture = LibraryCommandsFixture{libraryFixture.library(), changes};

    auto mutated = std::vector<TrackId>{};
    auto sub = changes.onChanged([&](LibraryChangeSet const& event) noexcept { mutated = event.tracksMutated; });

    auto const targetIds = std::array{trackId};
    auto const result = commandsFixture.updateMetadata(targetIds, MetadataPatch{.optTitle = "Original Title"});

    REQUIRE(result);
    CHECK(result->changes.empty());
    CHECK(mutated.empty());
  }

  TEST_CASE("LibraryCommands - updateMetadata compares canonical Unicode identity", "[runtime][unit][library][unicode]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const trackId = libraryFixture.addTrack("Café");
    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto commandsFixture = LibraryCommandsFixture{libraryFixture.library(), changes};
    auto mutated = std::vector<TrackId>{};
    auto sub = changes.onChanged([&](LibraryChangeSet const& event) noexcept { mutated = event.tracksMutated; });

    auto const result = commandsFixture.updateMetadata(std::array{trackId}, MetadataPatch{.optTitle = "Cafe\u0301"});

    REQUIRE(result);
    CHECK(result->changes.empty());
    CHECK(mutated.empty());
  }

  TEST_CASE("LibraryCommands - updateMetadata accepts complete metadata patches", "[runtime][unit][library][mutation]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const trackId = libraryFixture.addTrack("Original Title");
    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto commandsFixture = LibraryCommandsFixture{libraryFixture.library(), changes};

    auto const targetIds = std::array{trackId};
    auto const patch = MetadataPatch{.optTitle = "New Title",
                                     .optArtist = "Artist",
                                     .optAlbum = "Album",
                                     .optAlbumArtist = "Album Artist",
                                     .optGenre = "Rock",
                                     .optComposer = "Composer",
                                     .optConductor = "Conductor",
                                     .optEnsemble = "Ensemble",
                                     .optWork = "Work",
                                     .optMovement = "Movement",
                                     .optSoloist = "Soloist",
                                     .optYear = 2024,
                                     .optTrackNumber = 1,
                                     .optTrackTotal = 10,
                                     .optDiscNumber = 1,
                                     .optDiscTotal = 2};

    auto const result = commandsFixture.updateMetadata(targetIds, patch);
    REQUIRE(result);
    CHECK_FALSE(result->changes.empty());

    auto const transaction = libraryFixture.library().readTransaction();
    auto const optView =
      libraryFixture.library().tracks().reader(transaction).get(trackId, library::TrackStore::Reader::LoadMode::Both);
    REQUIRE(optView);
    auto const& dictionary = libraryFixture.library().dictionary();
    CHECK(dictionary.get(optView->classical().conductorId()) == "Conductor");
    CHECK(dictionary.get(optView->classical().ensembleId()) == "Ensemble");
    CHECK(dictionary.get(optView->classical().movementId()) == "Movement");
    CHECK(dictionary.get(optView->classical().soloistId()) == "Soloist");
  }

  TEST_CASE("LibraryCommands - updateMetadata applies and removes custom metadata",
            "[runtime][unit][library][mutation]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const trackId = libraryFixture.addTrack("Track");
    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto commandsFixture = LibraryCommandsFixture{libraryFixture.library(), changes};

    auto const targetIds = std::array{trackId};

    SECTION("Add/Update custom key")
    {
      auto patch = MetadataPatch{};
      patch.customUpdates["MyKey"] = "MyValue";
      REQUIRE(commandsFixture.updateMetadata(targetIds, patch));

      auto const transaction = libraryFixture.library().readTransaction();
      auto const optView =
        libraryFixture.library().tracks().reader(transaction).get(trackId, library::TrackStore::Reader::LoadMode::Both);
      REQUIRE(optView);
      auto const custom = optView->customMetadata();
      CHECK(std::ranges::distance(custom) == 1);
      auto const first = *custom.begin();
      CHECK(libraryFixture.library().dictionary().get(first.first) == "MyKey");
      CHECK(first.second == "MyValue");
    }

    SECTION("Remove custom key")
    {
      // First add it
      {
        auto patch = MetadataPatch{};
        patch.customUpdates["ToDelete"] = "Value";
        REQUIRE(commandsFixture.updateMetadata(targetIds, patch));
      }

      // Then remove it
      {
        auto patch = MetadataPatch{};
        patch.customUpdates["ToDelete"] = std::nullopt;
        REQUIRE(commandsFixture.updateMetadata(targetIds, patch));
      }

      auto const transaction = libraryFixture.library().readTransaction();
      auto const optView =
        libraryFixture.library().tracks().reader(transaction).get(trackId, library::TrackStore::Reader::LoadMode::Both);
      REQUIRE(optView);
      CHECK(std::ranges::distance(optView->customMetadata()) == 0);
    }
  }

  TEST_CASE("LibraryCommands - custom metadata uses canonical key and value identity",
            "[runtime][unit][library][unicode]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const trackId = libraryFixture.addTrack("Track");
    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto commandsFixture = LibraryCommandsFixture{libraryFixture.library(), changes};
    auto const targetIds = std::array{trackId};

    auto add = MetadataPatch{};
    add.customUpdates["Résumé"] = "Café";
    REQUIRE(commandsFixture.updateMetadata(targetIds, add));

    auto equivalent = MetadataPatch{};
    equivalent.customUpdates["Re\u0301sume\u0301"] = "Cafe\u0301";
    auto const equivalentRes = commandsFixture.updateMetadata(targetIds, equivalent);
    REQUIRE(equivalentRes);
    CHECK(equivalentRes->changes.empty());

    auto remove = MetadataPatch{};
    remove.customUpdates["Re\u0301sume\u0301"] = std::nullopt;
    REQUIRE(commandsFixture.updateMetadata(targetIds, remove));

    auto const transaction = libraryFixture.library().readTransaction();
    auto const optView =
      libraryFixture.library().tracks().reader(transaction).get(trackId, library::TrackStore::Reader::LoadMode::Both);
    REQUIRE(optView);
    CHECK(std::ranges::distance(optView->customMetadata()) == 0);
  }

  TEST_CASE("LibraryCommands - updateMetadata returns storage errors without committing",
            "[runtime][unit][library][mutation]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const trackId = libraryFixture.addTrack("Track");
    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto commandsFixture = LibraryCommandsFixture{libraryFixture.library(), changes};

    auto mutated = std::vector<TrackId>{};
    auto sub = changes.onChanged([&](LibraryChangeSet const& event) noexcept { mutated = event.tracksMutated; });

    auto patch = MetadataPatch{};
    patch.customUpdates["oversized"] = std::string(70'000, 'x');

    auto const result = commandsFixture.updateMetadata(std::array{trackId}, patch);

    REQUIRE_FALSE(result);
    CHECK(result.error().code == Error::Code::ValueTooLarge);
    CHECK(result.error().message.contains("Failed to update track data"));
    CHECK(result.error().message.contains("exceeds uint16_t"));

    CHECK(mutated.empty());

    auto const transaction = libraryFixture.library().readTransaction();
    auto const optView =
      libraryFixture.library().tracks().reader(transaction).get(trackId, library::TrackStore::Reader::LoadMode::Both);
    REQUIRE(optView);
    CHECK(std::ranges::distance(optView->customMetadata()) == 0);
  }

  TEST_CASE("LibraryCommands - editTags reports mutations for tag edits", "[runtime][unit][library][tag]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const trackId = libraryFixture.addTrack("Track");
    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto commandsFixture = LibraryCommandsFixture{libraryFixture.library(), changes};

    auto const trackIdsArr = std::array{trackId};
    auto const toAdd = std::array{std::string{"rock"}};
    auto const toRemove = std::array{std::string{"pop"}};
    auto const result = commandsFixture.editTags(trackIdsArr, toAdd, toRemove);

    REQUIRE(result);
    CHECK_FALSE(result->changes.empty());
  }

  TEST_CASE("LibraryCommands - editTags rejects missing targets before mutation", "[runtime][unit][library][tag]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto commandsFixture = LibraryCommandsFixture{libraryFixture.library(), changes};

    auto const trackIdsArr = std::array{TrackId{999}};
    auto const toAdd = std::array{std::string{"rock"}};
    auto const result = commandsFixture.editTags(trackIdsArr, toAdd, {});
    REQUIRE_FALSE(result);
    CHECK(result.error().code == Error::Code::NotFound);
  }

  TEST_CASE("LibraryCommands - lists can be created and updated", "[runtime][unit][library][list]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto commandsFixture = LibraryCommandsFixture{libraryFixture.library(), changes};
    auto& commands = commandsFixture.commands();

    auto draft = ListDraft{};
    draft.name = "List";
    draft.expression = R"(#favorite)";

    auto const listId = ao::test::requireValue(commandsFixture.runTask(commands.createList(draft)));
    CHECK(listId != kInvalidListId);

    auto updateDraft = ListDraft{};
    updateDraft.listId = listId;
    updateDraft.name = "Updated";
    updateDraft.expression = R"(#favorite or #recent)";
    auto const updateRes = commandsFixture.runTask(commands.updateList(updateDraft));
    REQUIRE(updateRes);

    auto const optNode = commandsFixture.library().snapshot().listNode(listId);
    REQUIRE(optNode);
    CHECK(optNode->name == "Updated");
    CHECK(optNode->expression == R"(#favorite or #recent)");
  }

  TEST_CASE("LibraryCommands - List display identity is NFC while filter bytes remain opaque",
            "[runtime][unit][library][unicode]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto commandsFixture = LibraryCommandsFixture{libraryFixture.library(), changes};
    auto& commands = commandsFixture.commands();
    auto const expression = std::string{"$title = 'Cafe\u0301'"};
    auto listIdRes = commandsFixture.runTask(
      commands.createList(ListDraft{.name = "Café", .description = "Crème", .expression = expression}));
    REQUIRE(listIdRes);
    auto const listId = *listIdRes;

    auto updateRes = commandsFixture.runTask(commands.updateList(ListDraft{
      .listId = listId,
      .name = "Cafe\u0301",
      .description = "Cre\u0300me",
      .expression = expression,
    }));

    REQUIRE(updateRes);
    CHECK_FALSE(updateRes->changed);
    auto const optNode = commandsFixture.library().snapshot().listNode(listId);
    REQUIRE(optNode);
    CHECK(optNode->name == "Café");
    CHECK(optNode->description == "Crème");
    CHECK(optNode->expression == expression);
  }

  TEST_CASE("LibraryCommands - updateList publishes ListsMutated", "[runtime][unit][library][mutation]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto commandsFixture = LibraryCommandsFixture{libraryFixture.library(), changes};
    auto& commands = commandsFixture.commands();

    auto draft = ListDraft{};
    draft.name = "Original";
    auto const listId = ao::test::requireValue(commandsFixture.runTask(commands.createList(draft)));

    auto upserted = std::vector<ListId>{};
    auto sub = changes.onChanged([&](LibraryChangeSet const& ev) noexcept { upserted = ev.listsUpserted; });

    auto updateDraft = ListDraft{};
    updateDraft.listId = listId;
    updateDraft.name = "Updated";
    auto const updateRes = commandsFixture.runTask(commands.updateList(updateDraft));
    REQUIRE(updateRes);

    REQUIRE(upserted.size() == 1);
    CHECK(upserted[0] == listId);
  }

  TEST_CASE("LibraryCommands - rejects invalid list drafts", "[runtime][unit][library][list]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto commandsFixture = LibraryCommandsFixture{libraryFixture.library(), changes};
    auto& commands = commandsFixture.commands();

    SECTION("invalid smart filter")
    {
      auto draft = ListDraft{};
      draft.name = "Invalid";
      draft.expression = "(";

      auto const result = commandsFixture.runTask(commands.createList(draft));
      REQUIRE(!result);
      CHECK(result.error().code == Error::Code::FormatRejected);
      CHECK(result.error().message.contains("invalid list filter"));
      CHECK(libraryFixture.library().lists().reader(libraryFixture.library().readTransaction()).begin() ==
            library::ListStore::Reader::Iterator{});
    }

    SECTION("empty expression matches the parent source")
    {
      auto draft = ListDraft{};
      draft.name = "Empty";

      auto const result = commandsFixture.runTask(commands.createList(draft));
      REQUIRE(result);

      auto const optNode = commandsFixture.library().snapshot().listNode(*result);
      REQUIRE(optNode);
      CHECK(optNode->expression.empty());
    }

    SECTION("missing parent")
    {
      auto draft = ListDraft{};
      draft.name = "Child";
      draft.parentId = ListId{999};

      auto const result = commandsFixture.runTask(commands.createList(draft));
      REQUIRE(!result);
      CHECK(result.error().code == Error::Code::InvalidInput);
      CHECK(result.error().message.contains("list parent not found"));
    }

    SECTION("self parent")
    {
      auto draft = ListDraft{};
      draft.name = "List";
      auto const listId = ao::test::requireValue(commandsFixture.runTask(commands.createList(draft)));

      draft.listId = listId;
      draft.parentId = listId;
      auto const result = commandsFixture.runTask(commands.updateList(draft));
      REQUIRE(!result);
      CHECK(result.error().code == Error::Code::InvalidInput);
      CHECK(result.error().message.contains("list parent cannot be the list itself"));
    }

    SECTION("descendant parent")
    {
      auto parentDraft = ListDraft{};
      parentDraft.name = "Parent";
      auto const parentId = ao::test::requireValue(commandsFixture.runTask(commands.createList(parentDraft)));

      auto childDraft = ListDraft{};
      childDraft.name = "Child";
      childDraft.parentId = parentId;
      auto const childId = ao::test::requireValue(commandsFixture.runTask(commands.createList(childDraft)));

      auto grandchildDraft = ListDraft{};
      grandchildDraft.name = "Grandchild";
      grandchildDraft.parentId = childId;
      auto const grandchildId = ao::test::requireValue(commandsFixture.runTask(commands.createList(grandchildDraft)));

      parentDraft.listId = parentId;
      parentDraft.parentId = grandchildId;
      auto const result = commandsFixture.runTask(commands.updateList(parentDraft));
      REQUIRE(!result);
      CHECK(result.error().code == Error::Code::InvalidInput);
      CHECK(result.error().message.contains("list parent cannot be a descendant of the list"));
    }
  }

  TEST_CASE("LibraryCommands - updateList skips unchanged drafts", "[runtime][unit][library][list]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto commandsFixture = LibraryCommandsFixture{libraryFixture.library(), changes};
    auto& commands = commandsFixture.commands();

    auto draft = ListDraft{};
    draft.name = "List";
    auto const listId = ao::test::requireValue(commandsFixture.runTask(commands.createList(draft)));
    draft.listId = listId;

    auto upserted = std::vector<ListId>{};
    auto sub = changes.onChanged([&](LibraryChangeSet const& ev) noexcept { upserted = ev.listsUpserted; });

    auto const updateRes = commandsFixture.runTask(commands.updateList(draft));
    REQUIRE(updateRes);
    CHECK(upserted.empty());
  }

  TEST_CASE("LibraryCommands - updateList reports missing lists as NotFound", "[runtime][unit][library][list]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto commandsFixture = LibraryCommandsFixture{libraryFixture.library(), changes};
    auto& commands = commandsFixture.commands();

    auto draft = ListDraft{};
    draft.listId = ListId{999};
    draft.name = "Missing";

    auto const result = commandsFixture.runTask(commands.updateList(draft));
    REQUIRE(!result);
    CHECK(result.error().code == Error::Code::NotFound);
    CHECK(result.error().message.contains("list not found: 999"));
  }

  TEST_CASE("LibraryCommands - deleteList publishes ListsMutated", "[runtime][unit][library][mutation]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto commandsFixture = LibraryCommandsFixture{libraryFixture.library(), changes};
    auto& commands = commandsFixture.commands();

    auto draft = ListDraft{};
    draft.name = "ToDelete";
    auto const listId = ao::test::requireValue(commandsFixture.runTask(commands.createList(draft)));

    auto deleted = std::vector<ListId>{};
    auto sub = changes.onChanged([&](LibraryChangeSet const& ev) noexcept { deleted = ev.listsDeleted; });

    REQUIRE(commandsFixture.runTask(commands.deleteList(listId)));

    REQUIRE(deleted.size() == 1);
    CHECK(deleted[0] == listId);
  }

  TEST_CASE("LibraryCommands - subtree delete previews and commits every descendant atomically",
            "[runtime][unit][list-delete][delete-subtree]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto commandsFixture = LibraryCommandsFixture{libraryFixture.library(), changes};
    auto& commands = commandsFixture.commands();
    auto const parentId =
      ao::test::requireValue(commandsFixture.runTask(commands.createList(ListDraft{.name = "Parent"})));
    auto const childId = ao::test::requireValue(
      commandsFixture.runTask(commands.createList(ListDraft{.parentId = parentId, .name = "Child"})));
    auto const grandchildId = ao::test::requireValue(
      commandsFixture.runTask(commands.createList(ListDraft{.parentId = childId, .name = "Grandchild"})));
    auto const unrelatedId =
      ao::test::requireValue(commandsFixture.runTask(commands.createList(ListDraft{.name = "Unrelated"})));

    auto const ordinaryDeleteRes = commandsFixture.runTask(commands.deleteList(parentId));
    REQUIRE_FALSE(ordinaryDeleteRes);
    CHECK(ordinaryDeleteRes.error().code == Error::Code::Conflict);
    CHECK(ordinaryDeleteRes.error().message.contains("Child"));

    auto const previewRes = commandsFixture.runTask(commands.previewDeleteListAndDescendants(parentId));
    REQUIRE(previewRes);
    CHECK(previewRes->rootListId == parentId);
    REQUIRE(previewRes->deletedLists.size() == 3);
    CHECK(previewRes->deletedLists[0].listId == parentId);
    CHECK(previewRes->deletedLists[1].listId == childId);
    CHECK(previewRes->deletedLists[2].listId == grandchildId);
    CHECK(commandsFixture.library().snapshot().listNode(parentId).has_value());
    CHECK(commandsFixture.library().snapshot().listNode(childId).has_value());
    CHECK(commandsFixture.library().snapshot().listNode(grandchildId).has_value());

    auto events = std::vector<LibraryChangeSet>{};
    auto sub = changes.onChanged([&events](LibraryChangeSet const& event) noexcept { events.push_back(event); });
    auto const result = commandsFixture.runTask(commands.deleteListAndDescendants(parentId));

    REQUIRE(result);
    CHECK(result->deletedLists == previewRes->deletedLists);
    REQUIRE(events.size() == 1);
    CHECK(events.front().listsDeleted == std::vector{parentId, childId, grandchildId});
    CHECK_FALSE(commandsFixture.library().snapshot().listNode(parentId).has_value());
    CHECK_FALSE(commandsFixture.library().snapshot().listNode(childId).has_value());
    CHECK_FALSE(commandsFixture.library().snapshot().listNode(grandchildId).has_value());
    CHECK(commandsFixture.library().snapshot().listNode(unrelatedId).has_value());
  }
} // namespace ao::rt::test
