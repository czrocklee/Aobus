// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "test/unit/RuntimeTestUtils.h"
#include <ao/Exception.h>
#include <ao/Type.h>
#include <ao/rt/StateTypes.h>
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

    CHECK_FALSE(result.mutatedIds.empty());
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

    CHECK_FALSE(result.mutatedIds.empty());
    CHECK(observedTitle == "Committed Title");
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
                                     .optWork = "Work",
                                     .optYear = 2024,
                                     .optTrackNumber = 1,
                                     .optTrackTotal = 10,
                                     .optDiscNumber = 1,
                                     .optDiscTotal = 2};

    auto const result = service.updateMetadata(targetIds, patch);
    CHECK_FALSE(result.mutatedIds.empty());
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
      service.updateMetadata(targetIds, patch);

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
        service.updateMetadata(targetIds, patch);
      }

      // Then remove it
      {
        auto patch = MetadataPatch{};
        patch.customUpdates["ToDelete"] = std::nullopt;
        service.updateMetadata(targetIds, patch);
      }

      auto const txn = testLib.library().readTransaction();
      auto const optView =
        testLib.library().tracks().reader(txn).get(trackId, library::TrackStore::Reader::LoadMode::Both);
      REQUIRE(optView);
      CHECK(std::ranges::distance(optView->customMetadata()) == 0);
    }
  }

  TEST_CASE("LibraryWriter - updateMetadata throws on serialization failure", "[runtime][unit][library][mutation]")
  {
    auto testLib = TestMusicLibrary{};
    auto const trackId = testLib.addTrack("Track");
    auto changes = LibraryChanges{};
    auto service = LibraryWriter{testLib.library(), changes};

    auto mutated = std::vector<TrackId>{};
    auto sub = changes.onTracksMutated([&](auto const& trackIds) { mutated = trackIds; });

    auto patch = MetadataPatch{};
    patch.customUpdates["oversized"] = std::string(70'000, 'x');

    try
    {
      std::ignore = service.updateMetadata(std::array{trackId}, patch);
      FAIL("updateMetadata should throw when track serialization fails");
    }
    catch (Exception const& e)
    {
      auto const message = std::string_view{e.what()};
      CHECK(message.find("Failed to serialize cold track data") != std::string_view::npos);
      CHECK(message.find("exceeds uint16_t") != std::string_view::npos);
    }

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

    CHECK_FALSE(result.mutatedIds.empty());
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
    CHECK(result.mutatedIds.empty());
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

    auto const listId = service.createList(draft);
    CHECK(listId != kInvalidListId);

    auto updateDraft = LibraryWriter::ListDraft{};
    updateDraft.listId = listId;
    updateDraft.name = "Updated";
    updateDraft.kind = LibraryWriter::ListKind::Manual;
    updateDraft.trackIds = {t1, t1};
    service.updateList(updateDraft);
  }

  TEST_CASE("LibraryWriter - updateList publishes ListsMutated", "[runtime][unit][library][mutation]")
  {
    auto testLib = TestMusicLibrary{};
    auto changes = LibraryChanges{};
    auto service = LibraryWriter{testLib.library(), changes};

    auto draft = LibraryWriter::ListDraft{};
    draft.name = "Original";
    auto const listId = service.createList(draft);

    auto upserted = std::vector<ListId>{};
    auto sub = changes.onListsMutated([&](auto const& ev) { upserted = ev.upserted; });

    auto updateDraft = LibraryWriter::ListDraft{};
    updateDraft.listId = listId;
    updateDraft.name = "Updated";
    service.updateList(updateDraft);

    REQUIRE(upserted.size() == 1);
    CHECK(upserted[0] == listId);
  }

  TEST_CASE("LibraryWriter - deleteList publishes ListsMutated", "[runtime][unit][library][mutation]")
  {
    auto testLib = TestMusicLibrary{};
    auto changes = LibraryChanges{};
    auto service = LibraryWriter{testLib.library(), changes};

    auto draft = LibraryWriter::ListDraft{};
    draft.name = "ToDelete";
    auto const listId = service.createList(draft);

    auto deleted = std::vector<ListId>{};
    auto sub = changes.onListsMutated([&](auto const& ev) { deleted = ev.deleted; });

    service.deleteList(listId);

    REQUIRE(deleted.size() == 1);
    CHECK(deleted[0] == listId);
  }
} // namespace ao::rt::test
