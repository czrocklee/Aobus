// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/RuntimeTestUtils.h"
#include <ao/CoreIds.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/ListBuilder.h>
#include <ao/library/ListStore.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackStore.h>
#include <ao/rt/CorePrimitives.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryWriter.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    struct ChangeRecorder final
    {
      explicit ChangeRecorder(LibraryChanges& changes)
        : tracksSub{changes.onTracksMutated([this](auto const&) { ++tracksMutated; })}
        , collectionSub{changes.onTrackCollectionChanged([this](auto const&) { ++collectionChanged; })}
        , listsSub{changes.onListsMutated([this](auto const&) { ++listsMutated; })}
      {
      }

      std::int32_t tracksMutated = 0;
      std::int32_t collectionChanged = 0;
      std::int32_t listsMutated = 0;

      Subscription tracksSub;
      Subscription collectionSub;
      Subscription listsSub;
    };

    bool trackExists(TestMusicLibrary& testLib, TrackId trackId)
    {
      auto txn = testLib.library().readTransaction();
      return static_cast<bool>(
        testLib.library().tracks().reader(txn).get(trackId, library::TrackStore::Reader::LoadMode::Both));
    }

    std::size_t trackCount(TestMusicLibrary& testLib)
    {
      std::size_t count = 0;
      auto txn = testLib.library().readTransaction();

      for ([[maybe_unused]] auto const& item : testLib.library().tracks().reader(txn))
      {
        ++count;
      }

      return count;
    }

    std::string trackTitle(TestMusicLibrary& testLib, TrackId trackId)
    {
      auto txn = testLib.library().readTransaction();
      auto const optView =
        testLib.library().tracks().reader(txn).get(trackId, library::TrackStore::Reader::LoadMode::Hot);
      REQUIRE(optView);
      return std::string{optView->metadata().title()};
    }

    bool trackHasTag(TestMusicLibrary& testLib, TrackId trackId, std::string_view tag)
    {
      auto txn = testLib.library().readTransaction();
      auto const optView =
        testLib.library().tracks().reader(txn).get(trackId, library::TrackStore::Reader::LoadMode::Hot);
      REQUIRE(optView);
      auto builder = library::TrackBuilder::fromView(*optView, testLib.library().dictionary());
      return std::ranges::contains(builder.tags().names(), tag);
    }

    bool listExists(TestMusicLibrary& testLib, ListId listId)
    {
      auto txn = testLib.library().readTransaction();
      return static_cast<bool>(testLib.library().lists().reader(txn).get(listId));
    }

    std::size_t listCount(TestMusicLibrary& testLib)
    {
      std::size_t count = 0;
      auto txn = testLib.library().readTransaction();

      for ([[maybe_unused]] auto const& item : testLib.library().lists().reader(txn))
      {
        ++count;
      }

      return count;
    }

    std::string listName(TestMusicLibrary& testLib, ListId listId)
    {
      auto txn = testLib.library().readTransaction();
      auto const optView = testLib.library().lists().reader(txn).get(listId);
      REQUIRE(optView);
      return std::string{optView->name()};
    }

    bool listContainsTrack(TestMusicLibrary& testLib, ListId listId, TrackId trackId)
    {
      auto txn = testLib.library().readTransaction();
      auto const optView = testLib.library().lists().reader(txn).get(listId);
      REQUIRE(optView);
      return std::ranges::contains(optView->tracks(), trackId);
    }

    ListId createManualList(TestMusicLibrary& testLib, std::string_view name, std::vector<TrackId> trackIds = {})
    {
      auto txn = testLib.library().writeTransaction();
      auto builder = library::ListBuilder::createNew().name(name);

      for (auto const trackId : trackIds)
      {
        builder.tracks().add(trackId);
      }

      auto const createResult = testLib.library().lists().writer(txn).create(builder.serialize());
      REQUIRE(createResult);
      REQUIRE(txn.commit());
      return createResult->first;
    }

    std::filesystem::path copyFixtureAudio(TestMusicLibrary const& testLib, std::string const& name)
    {
      auto const source = std::filesystem::current_path() / "test/integration/tag/test_data/empty.flac";

      if (!std::filesystem::exists(source))
      {
        return {};
      }

      auto const destination = testLib.root() / name;
      std::filesystem::create_directories(destination.parent_path());
      std::filesystem::copy_file(source, destination, std::filesystem::copy_options::overwrite_existing);
      return destination;
    }
  } // namespace

  TEST_CASE("LibraryWriter dry-run previews metadata updates without committing",
            "[runtime][unit][library][mutation][dryrun]")
  {
    auto testLib = TestMusicLibrary{};
    auto const trackId = testLib.addTrack("Before");
    auto changes = LibraryChanges{};
    auto writer = LibraryWriter{testLib.library(), changes};
    auto recorder = ChangeRecorder{changes};
    auto const patch = MetadataPatch{.optTitle = "After"};

    auto const dryRun = writer.previewUpdateMetadata(std::array{trackId}, patch);

    REQUIRE(dryRun);
    REQUIRE(dryRun->changes.size() == 1);
    REQUIRE(dryRun->changes[0].fields.size() == 1);
    CHECK(dryRun->changes[0].fields[0] ==
          TrackFieldChange{.field = "title", .oldValue = "Before", .newValue = "After"});
    CHECK(trackTitle(testLib, trackId) == "Before");
    CHECK(recorder.tracksMutated == 0);

    auto const commit = writer.updateMetadata(std::array{trackId}, patch);
    REQUIRE(commit);
    CHECK(*commit == *dryRun);
    CHECK(trackTitle(testLib, trackId) == "After");
    CHECK(recorder.tracksMutated == 1);
  }

  TEST_CASE("LibraryWriter dry-run previews tag edits without committing", "[runtime][unit][library][mutation][dryrun]")
  {
    auto testLib = TestMusicLibrary{};
    auto const trackId = testLib.addTrack("Track");
    auto changes = LibraryChanges{};
    auto writer = LibraryWriter{testLib.library(), changes};
    auto recorder = ChangeRecorder{changes};
    auto const tags = std::array{std::string{"Favorite"}};

    auto const dryRun = writer.previewEditTags(std::array{trackId}, tags, {});

    REQUIRE(dryRun);
    REQUIRE(dryRun->changes.size() == 1);
    CHECK(dryRun->changes[0].addedTags == std::vector<std::string>{"Favorite"});
    CHECK_FALSE(trackHasTag(testLib, trackId, "Favorite"));
    CHECK(recorder.tracksMutated == 0);

    auto const commit = writer.editTags(std::array{trackId}, tags, {});
    REQUIRE(commit);
    CHECK(*commit == *dryRun);
    CHECK(trackHasTag(testLib, trackId, "Favorite"));
    CHECK(recorder.tracksMutated == 1);
  }

  TEST_CASE("LibraryWriter dry-run previews list creation without committing",
            "[runtime][unit][library][mutation][dryrun]")
  {
    auto testLib = TestMusicLibrary{};
    auto changes = LibraryChanges{};
    auto writer = LibraryWriter{testLib.library(), changes};
    auto recorder = ChangeRecorder{changes};
    auto draft = LibraryWriter::ListDraft{.kind = LibraryWriter::ListKind::Manual, .name = "Draft"};

    auto const dryRun = writer.previewCreateList(draft);

    REQUIRE(dryRun);
    CHECK(listCount(testLib) == 0);
    CHECK(recorder.listsMutated == 0);

    auto const commit = writer.createList(draft);
    REQUIRE(commit);
    CHECK(listExists(testLib, *commit));
    CHECK(recorder.listsMutated == 1);
  }

  TEST_CASE("LibraryWriter dry-run previews list updates without committing",
            "[runtime][unit][library][mutation][dryrun]")
  {
    auto testLib = TestMusicLibrary{};
    auto const trackId = testLib.addTrack("Track");
    auto const listId = createManualList(testLib, "Before");
    auto changes = LibraryChanges{};
    auto writer = LibraryWriter{testLib.library(), changes};
    auto recorder = ChangeRecorder{changes};
    auto draft = LibraryWriter::ListDraft{
      .kind = LibraryWriter::ListKind::Manual, .listId = listId, .name = "After", .trackIds = {trackId}};

    auto const dryRun = writer.previewUpdateList(draft);

    REQUIRE(dryRun);
    CHECK(dryRun->changed);
    CHECK(dryRun->fieldChanges[0] == ListFieldChange{.field = "name", .oldValue = "Before", .newValue = "After"});
    CHECK(dryRun->addedTrackIds == std::vector<TrackId>{trackId});
    CHECK(listName(testLib, listId) == "Before");
    CHECK_FALSE(listContainsTrack(testLib, listId, trackId));
    CHECK(recorder.listsMutated == 0);

    auto const commit = writer.updateList(draft);
    REQUIRE(commit);
    CHECK(*commit == *dryRun);
    CHECK(listName(testLib, listId) == "After");
    CHECK(listContainsTrack(testLib, listId, trackId));
    CHECK(recorder.listsMutated == 1);
  }

  TEST_CASE("LibraryWriter dry-run previews list deletion without committing",
            "[runtime][unit][library][mutation][dryrun]")
  {
    auto testLib = TestMusicLibrary{};
    auto const trackId = testLib.addTrack("Track");
    auto const listId = createManualList(testLib, "Delete Me", {trackId});
    auto changes = LibraryChanges{};
    auto writer = LibraryWriter{testLib.library(), changes};
    auto recorder = ChangeRecorder{changes};

    auto const dryRun = writer.previewDeleteList(listId);

    REQUIRE(dryRun);
    CHECK(dryRun->name == "Delete Me");
    CHECK(dryRun->kind == "manual");
    CHECK(dryRun->trackCount == 1);
    CHECK(listExists(testLib, listId));
    CHECK(recorder.listsMutated == 0);

    auto const commit = writer.deleteList(listId);
    REQUIRE(commit);
    CHECK(*commit == *dryRun);
    CHECK_FALSE(listExists(testLib, listId));
    CHECK(recorder.listsMutated == 1);
  }

  TEST_CASE("LibraryWriter dry-run previews track deletion without committing",
            "[runtime][unit][library][mutation][dryrun]")
  {
    auto testLib = TestMusicLibrary{};
    auto const trackId = testLib.addTrack("Delete Track");
    auto const listId = createManualList(testLib, "Manual", {trackId});
    auto changes = LibraryChanges{};
    auto writer = LibraryWriter{testLib.library(), changes};
    auto recorder = ChangeRecorder{changes};

    auto const dryRun = writer.previewDeleteTrack(trackId);

    REQUIRE(dryRun);
    CHECK(dryRun->trackId == trackId);
    CHECK(dryRun->title == "Delete Track");
    CHECK(dryRun->removedFromListIds == std::vector<ListId>{listId});
    CHECK(trackExists(testLib, trackId));
    CHECK(listContainsTrack(testLib, listId, trackId));
    CHECK(recorder.tracksMutated == 0);
    CHECK(recorder.collectionChanged == 0);
    CHECK(recorder.listsMutated == 0);

    auto const commit = writer.deleteTrack(trackId);
    REQUIRE(commit);
    CHECK(*commit == *dryRun);
    CHECK_FALSE(trackExists(testLib, trackId));
    CHECK_FALSE(listContainsTrack(testLib, listId, trackId));
    CHECK(recorder.tracksMutated == 1);
    CHECK(recorder.collectionChanged == 1);
    CHECK(recorder.listsMutated == 1);
  }

  TEST_CASE("LibraryWriter dry-run previews track creation without committing",
            "[runtime][unit][library][mutation][dryrun]")
  {
    auto testLib = TestMusicLibrary{};
    auto changes = LibraryChanges{};
    auto writer = LibraryWriter{testLib.library(), changes};
    auto recorder = ChangeRecorder{changes};
    auto const absValidFile = copyFixtureAudio(testLib, "music/song.flac");

    if (!std::filesystem::exists(absValidFile))
    {
      SUCCEED("Skipping test because test file is missing");
      return;
    }

    auto const dryRun = writer.previewCreateTrackFromFile(absValidFile);

    REQUIRE(dryRun);
    CHECK(dryRun->uri == "music/song.flac");
    CHECK(trackCount(testLib) == 0);
    CHECK_FALSE(testLib.library().manifest().reader(testLib.library().readTransaction()).get("music/song.flac"));
    CHECK(recorder.tracksMutated == 0);
    CHECK(recorder.collectionChanged == 0);

    auto const commit = writer.createTrackFromFile(absValidFile);
    REQUIRE(commit);
    CHECK(commit->uri == dryRun->uri);
    CHECK(commit->title == dryRun->title);
    CHECK(commit->artist == dryRun->artist);
    CHECK(trackExists(testLib, commit->trackId));
    CHECK(testLib.library().manifest().reader(testLib.library().readTransaction()).get("music/song.flac"));
    CHECK(recorder.tracksMutated == 1);
    CHECK(recorder.collectionChanged == 1);
  }
} // namespace ao::rt::test
