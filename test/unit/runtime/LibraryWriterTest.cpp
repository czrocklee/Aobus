// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "TestUtils.h"
#include <ao/Type.h>
#include <ao/async/Runtime.h>
#include <ao/async/Task.h>
#include <ao/library/LibraryScanner.h>
#include <ao/rt/StateTypes.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryTasks.h>
#include <ao/rt/library/LibraryWriter.h>
#include <ao/rt/library/LibraryYamlExporter.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <functional>
#include <iterator>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ao::rt::test
{
  TEST_CASE("LibraryWriter - updateMetadata publishes TracksMutated", "[app][unit][runtime][mutation]")
  {
    auto testLib = TestMusicLibrary{};
    auto const trackId = testLib.addTrack("Original Title");

    auto changes = LibraryChanges{};
    auto service = LibraryWriter{testLib.library(), changes};

    auto mutated = std::vector<TrackId>{};
    auto sub = changes.onTracksMutated([&](auto const& trackIds) { mutated = trackIds; });

    auto const targetIds = std::array{trackId};
    auto const result = service.updateMetadata(targetIds, MetadataPatch{.optTitle = "New Title"});

    REQUIRE_FALSE(result.mutatedIds.empty());
    REQUIRE(mutated.size() == 1);
    CHECK(mutated[0] == trackId);
  }

  TEST_CASE("LibraryWriter - observers see committed metadata", "[app][unit][runtime][mutation]")
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

    REQUIRE_FALSE(result.mutatedIds.empty());
    CHECK(observedTitle == "Committed Title");
  }

  TEST_CASE("LibraryWriter - updateMetadata full patch", "[app][unit][runtime][mutation]")
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
    REQUIRE_FALSE(result.mutatedIds.empty());
  }

  TEST_CASE("LibraryWriter - updateMetadata custom metadata", "[app][unit][runtime][mutation]")
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

  TEST_CASE("LibraryWriter - editTags full operation", "[app][unit][runtime][mutation]")
  {
    auto testLib = TestMusicLibrary{};
    auto const trackId = testLib.addTrack("Track");
    auto changes = LibraryChanges{};
    auto service = LibraryWriter{testLib.library(), changes};

    auto const trackIdsArr = std::array{trackId};
    auto const toAdd = std::array{std::string{"rock"}};
    auto const toRemove = std::array{std::string{"pop"}};
    auto const result = service.editTags(trackIdsArr, toAdd, toRemove);

    REQUIRE_FALSE(result.mutatedIds.empty());
  }

  TEST_CASE("LibraryWriter - editTags missing track continues", "[app][unit][runtime][mutation]")
  {
    auto testLib = TestMusicLibrary{};
    auto changes = LibraryChanges{};
    auto service = LibraryWriter{testLib.library(), changes};

    auto const trackIdsArr = std::array{TrackId{999}};
    auto const toAdd = std::array{std::string{"rock"}};
    auto const result = service.editTags(trackIdsArr, toAdd, {});
    CHECK(result.mutatedIds.empty());
  }

  TEST_CASE("LibraryWriter - create/update Manual list", "[app][unit][runtime][mutation]")
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
    REQUIRE(listId != kInvalidListId);

    auto updateDraft = LibraryWriter::ListDraft{};
    updateDraft.listId = listId;
    updateDraft.name = "Updated";
    updateDraft.kind = LibraryWriter::ListKind::Manual;
    updateDraft.trackIds = {t1, t1};
    service.updateList(updateDraft);
  }

  TEST_CASE("LibraryWriter - updateList publishes ListsMutated", "[app][unit][runtime][mutation]")
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

  TEST_CASE("LibraryWriter - deleteList publishes ListsMutated", "[app][unit][runtime][mutation]")
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

  TEST_CASE("LibraryTasks - async tasks (dummy execution)", "[app][unit][runtime][tasks]")
  {
    auto testLib = TestMusicLibrary{};
    auto executor = MockExecutor{};
    auto runtime = async::Runtime{executor};
    auto changes = LibraryChanges{};
    auto service = LibraryTasks{runtime, testLib.library(), changes};

    SECTION("importLibraryAsync with invalid path throws")
    {
      auto future = runtime.spawn(service.importLibraryAsync("/nonexistent_path_123.yaml"));
      REQUIRE_THROWS(future.get());
    }

    SECTION("exportLibraryAsync with invalid path throws")
    {
      auto future = runtime.spawn(service.exportLibraryAsync("/root/nonexistent_path_123.yaml", ExportMode::Full));
      REQUIRE_THROWS(future.get());
    }

    SECTION("buildScanPlanAsync executes")
    {
      auto wrapperTask = [](LibraryTasks* s) -> async::Task<void>
      {
        auto plan = co_await s->buildScanPlanAsync();
        co_return;
      };

      auto future = runtime.spawn(wrapperTask(&service));
      REQUIRE_NOTHROW(future.get());
    }

    SECTION("applyScanPlanAsync executes")
    {
      auto wrapperTask = [](LibraryTasks* s) -> async::Task<void>
      {
        auto plan = library::ScanPlan{};
        co_await s->applyScanPlanAsync(std::move(plan));
        co_return;
      };

      auto future = runtime.spawn(wrapperTask(&service));
      REQUIRE_NOTHROW(future.get());
    }

    SECTION("applyScanPlanAsync executes and reports progress")
    {
      bool progressFired = false;
      auto sub = changes.onLibraryTaskProgress(
        [&](auto const& ev)
        {
          progressFired = true;
          CHECK(ev.fraction >= 0.0);
        });

      auto wrapperTask = [](LibraryTasks* s) -> async::Task<void>
      {
        auto plan = library::ScanPlan{};
        auto item = library::ScanItem{};
        item.uri = "file:///fake/path.flac";
        item.fullPath = "/fake/path.flac";
        item.classification = library::ScanClassification::New;
        plan.items.push_back(item);
        co_await s->applyScanPlanAsync(std::move(plan));
        co_return;
      };

      auto future = runtime.spawn(wrapperTask(&service));
      REQUIRE_NOTHROW(future.get());
      CHECK(progressFired);
    }
  }
} // namespace ao::rt::test
