// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "TestUtils.h"
#include "ao/Type.h"
#include <ao/library/LibraryScanner.h>
#include <ao/rt/LibraryMutationService.h>
#include <ao/rt/LibraryYamlExporter.h>
#include <ao/rt/StateTypes.h>
#include <ao/rt/async/Runtime.h>
#include <ao/rt/async/Task.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <functional>
#include <utility>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    struct NullExecutor final : public IControlExecutor
    {
      bool isCurrent() const noexcept override { return true; }
      void dispatch(std::move_only_function<void()> task) override { task(); }
      void defer(std::move_only_function<void()> task) override { task(); }
    };
  }

  TEST_CASE("LibraryMutationService - updateMetadata publishes TracksMutated", "[app][unit][runtime][mutation]")
  {
    auto testLib = TestMusicLibrary{};
    auto const trackId = testLib.addTrack("Original Title");

    auto executor = NullExecutor{};
    auto runtime = async::Runtime{executor};
    auto service = LibraryMutationService{runtime, testLib.library()};

    auto mutated = std::vector<TrackId>{};
    auto sub = service.onTracksMutated([&](auto const& trackIds) { mutated = trackIds; });

    auto const targetIds = std::array{trackId};
    auto const result = service.updateMetadata(targetIds, MetadataPatch{.optTitle = "New Title"});

    REQUIRE(result.has_value());
    REQUIRE(mutated.size() == 1);
    CHECK(mutated[0] == trackId);
  }

  TEST_CASE("LibraryMutationService - updateMetadata full patch", "[app][unit][runtime][mutation]")
  {
    auto testLib = TestMusicLibrary{};
    auto const trackId = testLib.addTrack("Original Title");
    auto executor = NullExecutor{};
    auto runtime = async::Runtime{executor};
    auto service = LibraryMutationService{runtime, testLib.library()};

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
                                     .optTotalTracks = 10,
                                     .optDiscNumber = 1,
                                     .optTotalDiscs = 2};

    auto const result = service.updateMetadata(targetIds, patch);
    REQUIRE(result.has_value());
  }

  TEST_CASE("LibraryMutationService - editTags full operation", "[app][unit][runtime][mutation]")
  {
    auto testLib = TestMusicLibrary{};
    auto const trackId = testLib.addTrack("Track");
    auto executor = NullExecutor{};
    auto runtime = async::Runtime{executor};
    auto service = LibraryMutationService{runtime, testLib.library()};

    auto const trackIdsArr = std::array{trackId};
    auto const toAdd = std::array{std::string{"rock"}};
    auto const toRemove = std::array{std::string{"pop"}};
    auto const result = service.editTags(trackIdsArr, toAdd, toRemove);

    REQUIRE(result.has_value());
  }

  TEST_CASE("LibraryMutationService - editTags missing track continues", "[app][unit][runtime][mutation]")
  {
    auto testLib = TestMusicLibrary{};
    auto executor = NullExecutor{};
    auto runtime = async::Runtime{executor};
    auto service = LibraryMutationService{runtime, testLib.library()};

    auto const trackIdsArr = std::array{TrackId{999}};
    auto const toAdd = std::array{std::string{"rock"}};
    auto const result = service.editTags(trackIdsArr, toAdd, {});
    REQUIRE(result.has_value());
  }

  TEST_CASE("LibraryMutationService - create/update Manual list", "[app][unit][runtime][mutation]")
  {
    auto testLib = TestMusicLibrary{};
    auto const t1 = testLib.addTrack("A");
    auto executor = NullExecutor{};
    auto runtime = async::Runtime{executor};
    auto service = LibraryMutationService{runtime, testLib.library()};

    auto draft = LibraryMutationService::ListDraft{};
    draft.name = "Manual List";
    draft.kind = LibraryMutationService::ListKind::Manual;
    draft.trackIds = {t1};

    auto const listId = service.createList(draft);
    REQUIRE(listId != kInvalidListId);

    auto updateDraft = LibraryMutationService::ListDraft{};
    updateDraft.listId = listId;
    updateDraft.name = "Updated";
    updateDraft.kind = LibraryMutationService::ListKind::Manual;
    updateDraft.trackIds = {t1, t1};
    service.updateList(updateDraft);
  }

  TEST_CASE("LibraryMutationService - updateList publishes ListsMutated", "[app][unit][runtime][mutation]")
  {
    auto testLib = TestMusicLibrary{};
    auto executor = NullExecutor{};
    auto runtime = async::Runtime{executor};
    auto service = LibraryMutationService{runtime, testLib.library()};

    auto draft = LibraryMutationService::ListDraft{};
    draft.name = "Original";
    auto const listId = service.createList(draft);

    auto upserted = std::vector<ListId>{};
    auto sub = service.onListsMutated([&](auto const& ev) { upserted = ev.upserted; });

    auto updateDraft = LibraryMutationService::ListDraft{};
    updateDraft.listId = listId;
    updateDraft.name = "Updated";
    service.updateList(updateDraft);

    REQUIRE(upserted.size() == 1);
    CHECK(upserted[0] == listId);
  }

  TEST_CASE("LibraryMutationService - deleteList publishes ListsMutated", "[app][unit][runtime][mutation]")
  {
    auto testLib = TestMusicLibrary{};
    auto executor = NullExecutor{};
    auto runtime = async::Runtime{executor};
    auto service = LibraryMutationService{runtime, testLib.library()};

    auto draft = LibraryMutationService::ListDraft{};
    draft.name = "ToDelete";
    auto const listId = service.createList(draft);

    auto deleted = std::vector<ListId>{};
    auto sub = service.onListsMutated([&](auto const& ev) { deleted = ev.deleted; });

    service.deleteList(listId);

    REQUIRE(deleted.size() == 1);
    CHECK(deleted[0] == listId);
  }

  TEST_CASE("LibraryMutationService - direct notifications", "[app][unit][runtime][mutation]")
  {
    auto testLib = TestMusicLibrary{};
    auto executor = NullExecutor{};
    auto runtime = async::Runtime{executor};
    auto service = LibraryMutationService{runtime, testLib.library()};

    auto mutatedTracks = std::vector<TrackId>{};
    auto sub1 = service.onTracksMutated([&](auto const& ids) { mutatedTracks = ids; });

    auto upsertedLists = std::vector<ListId>{};
    auto deletedLists = std::vector<ListId>{};
    auto sub2 = service.onListsMutated(
      [&](auto const& ev)
      {
        upsertedLists = ev.upserted;
        deletedLists = ev.deleted;
      });

    auto taskCount = std::size_t{0};
    auto sub3 = service.onLibraryTaskCompleted([&](auto count) { taskCount = count; });

    service.notifyTracksMutated({TrackId{1}, TrackId{2}});
    REQUIRE(mutatedTracks.size() == 2);

    service.notifyListsMutated({ListId{1}}, {ListId{2}});
    REQUIRE(upsertedLists.size() == 1);
    REQUIRE(deletedLists.size() == 1);

    service.notifyLibraryTaskCompleted(42);
    REQUIRE(taskCount == 42);
  }

  TEST_CASE("LibraryMutationService - Async tasks (dummy execution)", "[app][unit][runtime][mutation]")
  {
    auto testLib = TestMusicLibrary{};
    auto executor = NullExecutor{};
    auto runtime = async::Runtime{executor};
    auto service = LibraryMutationService{runtime, testLib.library()};

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
      auto wrapperTask = [](LibraryMutationService* s) -> async::Task<void>
      {
        auto plan = co_await s->buildScanPlanAsync();
        co_return;
      };

      auto future = runtime.spawn(wrapperTask(&service));
      REQUIRE_NOTHROW(future.get());
    }

    SECTION("applyScanPlanAsync executes")
    {
      auto wrapperTask = [](LibraryMutationService* s) -> async::Task<void>
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
      auto sub = service.onLibraryTaskProgress(
        [&](auto const& ev)
        {
          progressFired = true;
          CHECK(ev.fraction >= 0.0);
        });

      auto wrapperTask = [](LibraryMutationService* s) -> async::Task<void>
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
