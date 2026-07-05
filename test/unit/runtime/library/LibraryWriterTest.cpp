// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/RuntimeTestUtils.h"
#include "test/unit/TestUtils.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/ListStore.h>
#include <ao/library/TrackStore.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryWriter.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <functional>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::rt::test
{
  TEST_CASE("LibraryWriter - updateMetadata publishes TracksMutated", "[runtime][unit][library][mutation]")
  {
    auto testLib = TestMusicLibrary{};
    auto const trackId = testLib.addTrack("Original Title");

    auto changes = LibraryChanges{};
    auto service = LibraryWriter{testLib.library(), changes};

    auto mutated = std::vector<TrackId>{};
    auto sub = changes.onTracksMutated([&](auto const& trackIds) { mutated = trackIds; });

    auto const targetIds = std::array{trackId};
    auto const result = service.updateMetadata(targetIds, MetadataPatch{.optTitle = "New Title"});

    REQUIRE(result);
    CHECK_FALSE(result->mutatedIds.empty());
    REQUIRE(mutated.size() == 1);
    CHECK(mutated[0] == trackId);
  }

  TEST_CASE("LibraryWriter - observers see committed metadata", "[runtime][unit][library][mutation]")
  {
    auto testLib = TestMusicLibrary{};
    auto const trackId = testLib.addTrack("Original Title");

    auto changes = LibraryChanges{};
    auto service = LibraryWriter{testLib.library(), changes};

    auto observedTitle = std::string{};
    auto sub = changes.onTracksMutated(
      [&](auto const& trackIds)
      {
        REQUIRE(trackIds.size() == 1);
        CHECK(trackIds[0] == trackId);

        auto txn = testLib.library().readTransaction();
        auto const optView =
          testLib.library().tracks().reader(txn).get(trackId, library::TrackStore::Reader::LoadMode::Hot);
        REQUIRE(optView);
        observedTitle = std::string{optView->metadata().title()};
      });

    auto const targetIds = std::array{trackId};
    auto const result = service.updateMetadata(targetIds, MetadataPatch{.optTitle = "Committed Title"});

    REQUIRE(result);
    CHECK_FALSE(result->mutatedIds.empty());
    CHECK(observedTitle == "Committed Title");
  }

  TEST_CASE("LibraryWriter - updateMetadata reports no mutation for identical values",
            "[runtime][unit][library][mutation]")
  {
    auto testLib = TestMusicLibrary{};
    auto const trackId = testLib.addTrack("Original Title");

    auto changes = LibraryChanges{};
    auto service = LibraryWriter{testLib.library(), changes};

    auto mutated = std::vector<TrackId>{};
    auto sub = changes.onTracksMutated([&](auto const& trackIds) { mutated = trackIds; });

    auto const targetIds = std::array{trackId};
    auto const result = service.updateMetadata(targetIds, MetadataPatch{.optTitle = "Original Title"});

    REQUIRE(result);
    CHECK(result->mutatedIds.empty());
    CHECK(mutated.empty());
  }

  TEST_CASE("LibraryWriter - updateMetadata accepts complete metadata patches", "[runtime][unit][library][mutation]")
  {
    auto testLib = TestMusicLibrary{};
    auto const trackId = testLib.addTrack("Original Title");
    auto changes = LibraryChanges{};
    auto service = LibraryWriter{testLib.library(), changes};

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

    auto const result = service.updateMetadata(targetIds, patch);
    REQUIRE(result);
    CHECK_FALSE(result->mutatedIds.empty());

    auto const txn = testLib.library().readTransaction();
    auto const optView =
      testLib.library().tracks().reader(txn).get(trackId, library::TrackStore::Reader::LoadMode::Both);
    REQUIRE(optView);
    auto const& dict = testLib.library().dictionary();
    CHECK(dict.get(optView->classical().conductorId()) == "Conductor");
    CHECK(dict.get(optView->classical().ensembleId()) == "Ensemble");
    CHECK(dict.get(optView->classical().movementId()) == "Movement");
    CHECK(dict.get(optView->classical().soloistId()) == "Soloist");
  }

  TEST_CASE("LibraryWriter - updateMetadata applies and removes custom metadata", "[runtime][unit][library][mutation]")
  {
    auto testLib = TestMusicLibrary{};
    auto const trackId = testLib.addTrack("Track");
    auto changes = LibraryChanges{};
    auto service = LibraryWriter{testLib.library(), changes};

    auto const targetIds = std::array{trackId};

    SECTION("Add/Update custom key")
    {
      auto patch = MetadataPatch{};
      patch.customUpdates["MyKey"] = "MyValue";
      REQUIRE(service.updateMetadata(targetIds, patch));

      auto const txn = testLib.library().readTransaction();
      auto const optView =
        testLib.library().tracks().reader(txn).get(trackId, library::TrackStore::Reader::LoadMode::Both);
      REQUIRE(optView);
      auto const custom = optView->customMetadata();
      CHECK(std::ranges::distance(custom) == 1);
      auto const first = *custom.begin();
      CHECK(testLib.library().dictionary().get(first.first) == "MyKey");
      CHECK(first.second == "MyValue");
    }

    SECTION("Remove custom key")
    {
      // First add it
      {
        auto patch = MetadataPatch{};
        patch.customUpdates["ToDelete"] = "Value";
        REQUIRE(service.updateMetadata(targetIds, patch));
      }

      // Then remove it
      {
        auto patch = MetadataPatch{};
        patch.customUpdates["ToDelete"] = std::nullopt;
        REQUIRE(service.updateMetadata(targetIds, patch));
      }

      auto const txn = testLib.library().readTransaction();
      auto const optView =
        testLib.library().tracks().reader(txn).get(trackId, library::TrackStore::Reader::LoadMode::Both);
      REQUIRE(optView);
      CHECK(std::ranges::distance(optView->customMetadata()) == 0);
    }
  }

  TEST_CASE("LibraryWriter - updateMetadata returns storage errors without committing",
            "[runtime][unit][library][mutation]")
  {
    auto testLib = TestMusicLibrary{};
    auto const trackId = testLib.addTrack("Track");
    auto changes = LibraryChanges{};
    auto service = LibraryWriter{testLib.library(), changes};

    auto mutated = std::vector<TrackId>{};
    auto sub = changes.onTracksMutated([&](auto const& trackIds) { mutated = trackIds; });

    auto patch = MetadataPatch{};
    patch.customUpdates["oversized"] = std::string(70'000, 'x');

    auto const result = service.updateMetadata(std::array{trackId}, patch);

    REQUIRE_FALSE(result);
    CHECK(result.error().code == Error::Code::ValueTooLarge);
    CHECK(result.error().message.find("Failed to serialize cold track data") != std::string::npos);
    CHECK(result.error().message.find("exceeds uint16_t") != std::string::npos);

    CHECK(mutated.empty());

    auto const txn = testLib.library().readTransaction();
    auto const optView =
      testLib.library().tracks().reader(txn).get(trackId, library::TrackStore::Reader::LoadMode::Both);
    REQUIRE(optView);
    CHECK(std::ranges::distance(optView->customMetadata()) == 0);
  }

  TEST_CASE("LibraryWriter - editTags reports mutations for tag edits", "[runtime][unit][library][mutation][tags]")
  {
    auto testLib = TestMusicLibrary{};
    auto const trackId = testLib.addTrack("Track");
    auto changes = LibraryChanges{};
    auto service = LibraryWriter{testLib.library(), changes};

    auto const trackIdsArr = std::array{trackId};
    auto const toAdd = std::array{std::string{"rock"}};
    auto const toRemove = std::array{std::string{"pop"}};
    auto const result = service.editTags(trackIdsArr, toAdd, toRemove);

    REQUIRE(result);
    CHECK_FALSE(result->mutatedIds.empty());
  }

  TEST_CASE("LibraryWriter - editTags skips missing tracks without mutations",
            "[runtime][unit][library][mutation][tags]")
  {
    auto testLib = TestMusicLibrary{};
    auto changes = LibraryChanges{};
    auto service = LibraryWriter{testLib.library(), changes};

    auto const trackIdsArr = std::array{TrackId{999}};
    auto const toAdd = std::array{std::string{"rock"}};
    auto const result = service.editTags(trackIdsArr, toAdd, {});
    REQUIRE(result);
    CHECK(result->mutatedIds.empty());
  }

  TEST_CASE("LibraryWriter - manual lists can be created and updated", "[runtime][unit][library][mutation][list]")
  {
    auto testLib = TestMusicLibrary{};
    auto const t1 = testLib.addTrack("A");
    auto changes = LibraryChanges{};
    auto service = LibraryWriter{testLib.library(), changes};

    auto draft = LibraryWriter::ListDraft{};
    draft.name = "Manual List";
    draft.kind = LibraryWriter::ListKind::Manual;
    draft.trackIds = {t1};

    auto const listId = ao::test::requireValue(service.createList(draft));
    CHECK(listId != kInvalidListId);

    auto updateDraft = LibraryWriter::ListDraft{};
    updateDraft.listId = listId;
    updateDraft.name = "Updated";
    updateDraft.kind = LibraryWriter::ListKind::Manual;
    updateDraft.trackIds = {t1, t1};
    auto const updateResult = service.updateList(updateDraft);
    REQUIRE(updateResult);
  }

  TEST_CASE("LibraryWriter - updateList publishes ListsMutated", "[runtime][unit][library][mutation]")
  {
    auto testLib = TestMusicLibrary{};
    auto changes = LibraryChanges{};
    auto service = LibraryWriter{testLib.library(), changes};

    auto draft = LibraryWriter::ListDraft{};
    draft.name = "Original";
    auto const listId = ao::test::requireValue(service.createList(draft));

    auto upserted = std::vector<ListId>{};
    auto sub = changes.onListsMutated([&](auto const& ev) { upserted = ev.upserted; });

    auto updateDraft = LibraryWriter::ListDraft{};
    updateDraft.listId = listId;
    updateDraft.name = "Updated";
    auto const updateResult = service.updateList(updateDraft);
    REQUIRE(updateResult);

    REQUIRE(upserted.size() == 1);
    CHECK(upserted[0] == listId);
  }

  TEST_CASE("LibraryWriter - rejects invalid list drafts", "[runtime][unit][library][mutation][list]")
  {
    auto testLib = TestMusicLibrary{};
    auto changes = LibraryChanges{};
    auto service = LibraryWriter{testLib.library(), changes};

    SECTION("invalid smart filter")
    {
      auto draft = LibraryWriter::ListDraft{};
      draft.kind = LibraryWriter::ListKind::Smart;
      draft.name = "Invalid";
      draft.expression = "(";

      auto const result = service.createList(draft);
      REQUIRE(!result);
      CHECK(result.error().code == Error::Code::FormatRejected);
      CHECK(result.error().message.find("invalid list filter") != std::string::npos);
      CHECK(testLib.library().lists().reader(testLib.library().readTransaction()).begin() ==
            library::ListStore::Reader::Iterator{});
    }

    SECTION("missing parent")
    {
      auto draft = LibraryWriter::ListDraft{};
      draft.kind = LibraryWriter::ListKind::Manual;
      draft.name = "Child";
      draft.parentId = ListId{999};

      auto const result = service.createList(draft);
      REQUIRE(!result);
      CHECK(result.error().code == Error::Code::InvalidInput);
      CHECK(result.error().message.find("list parent not found") != std::string::npos);
    }

    SECTION("self parent")
    {
      auto draft = LibraryWriter::ListDraft{};
      draft.kind = LibraryWriter::ListKind::Manual;
      draft.name = "List";
      auto const listId = ao::test::requireValue(service.createList(draft));

      draft.listId = listId;
      draft.parentId = listId;
      auto const result = service.updateList(draft);
      REQUIRE(!result);
      CHECK(result.error().code == Error::Code::InvalidInput);
      CHECK(result.error().message.find("list parent cannot be the list itself") != std::string::npos);
    }

    SECTION("descendant parent")
    {
      auto parentDraft = LibraryWriter::ListDraft{};
      parentDraft.kind = LibraryWriter::ListKind::Manual;
      parentDraft.name = "Parent";
      auto const parentId = ao::test::requireValue(service.createList(parentDraft));

      auto childDraft = LibraryWriter::ListDraft{};
      childDraft.kind = LibraryWriter::ListKind::Manual;
      childDraft.name = "Child";
      childDraft.parentId = parentId;
      auto const childId = ao::test::requireValue(service.createList(childDraft));

      auto grandchildDraft = LibraryWriter::ListDraft{};
      grandchildDraft.kind = LibraryWriter::ListKind::Manual;
      grandchildDraft.name = "Grandchild";
      grandchildDraft.parentId = childId;
      auto const grandchildId = ao::test::requireValue(service.createList(grandchildDraft));

      parentDraft.listId = parentId;
      parentDraft.parentId = grandchildId;
      auto const result = service.updateList(parentDraft);
      REQUIRE(!result);
      CHECK(result.error().code == Error::Code::InvalidInput);
      CHECK(result.error().message.find("list parent cannot be a descendant of the list") != std::string::npos);
    }
  }

  TEST_CASE("LibraryWriter - updateList skips unchanged drafts", "[runtime][unit][library][mutation][list]")
  {
    auto testLib = TestMusicLibrary{};
    auto changes = LibraryChanges{};
    auto service = LibraryWriter{testLib.library(), changes};

    auto draft = LibraryWriter::ListDraft{};
    draft.kind = LibraryWriter::ListKind::Manual;
    draft.name = "Manual";
    auto const listId = ao::test::requireValue(service.createList(draft));
    draft.listId = listId;

    auto upserted = std::vector<ListId>{};
    auto sub = changes.onListsMutated([&](auto const& ev) { upserted = ev.upserted; });

    auto const updateResult = service.updateList(draft);
    REQUIRE(updateResult);
    CHECK(upserted.empty());
  }

  TEST_CASE("LibraryWriter - updateList reports missing lists as NotFound", "[runtime][unit][library][mutation][list]")
  {
    auto testLib = TestMusicLibrary{};
    auto changes = LibraryChanges{};
    auto service = LibraryWriter{testLib.library(), changes};

    auto draft = LibraryWriter::ListDraft{};
    draft.kind = LibraryWriter::ListKind::Manual;
    draft.listId = ListId{999};
    draft.name = "Missing";

    auto const result = service.updateList(draft);
    REQUIRE(!result);
    CHECK(result.error().code == Error::Code::NotFound);
    CHECK(result.error().message.find("list not found: 999") != std::string::npos);
  }

  TEST_CASE("LibraryWriter - deleteList publishes ListsMutated", "[runtime][unit][library][mutation]")
  {
    auto testLib = TestMusicLibrary{};
    auto changes = LibraryChanges{};
    auto service = LibraryWriter{testLib.library(), changes};

    auto draft = LibraryWriter::ListDraft{};
    draft.name = "ToDelete";
    auto const listId = ao::test::requireValue(service.createList(draft));

    auto deleted = std::vector<ListId>{};
    auto sub = changes.onListsMutated([&](auto const& ev) { deleted = ev.deleted; });

    REQUIRE(service.deleteList(listId));

    REQUIRE(deleted.size() == 1);
    CHECK(deleted[0] == listId);
  }
} // namespace ao::rt::test
