// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/RuntimeTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/ListBuilder.h>
#include <ao/library/ListStore.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackStore.h>
#include <ao/rt/Subscription.h>
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

    bool trackExists(MusicLibraryFixture& libraryFixture, TrackId trackId)
    {
      auto transaction = libraryFixture.library().readTransaction();
      return libraryFixture.library()
        .tracks()
        .reader(transaction)
        .get(trackId, library::TrackStore::Reader::LoadMode::Both)
        .has_value();
    }

    std::size_t trackCount(MusicLibraryFixture& libraryFixture)
    {
      std::size_t count = 0;
      auto transaction = libraryFixture.library().readTransaction();

      for ([[maybe_unused]] auto const& item : libraryFixture.library().tracks().reader(transaction))
      {
        ++count;
      }

      return count;
    }

    std::string trackTitle(MusicLibraryFixture& libraryFixture, TrackId trackId)
    {
      auto transaction = libraryFixture.library().readTransaction();
      auto const optView =
        libraryFixture.library().tracks().reader(transaction).get(trackId, library::TrackStore::Reader::LoadMode::Hot);
      REQUIRE(optView);
      return std::string{optView->metadata().title()};
    }

    bool trackHasTag(MusicLibraryFixture& libraryFixture, TrackId trackId, std::string_view tag)
    {
      auto transaction = libraryFixture.library().readTransaction();
      auto const optView =
        libraryFixture.library().tracks().reader(transaction).get(trackId, library::TrackStore::Reader::LoadMode::Hot);
      REQUIRE(optView);
      auto builder = library::TrackBuilder::fromView(*optView, libraryFixture.library().dictionary());
      return std::ranges::contains(builder.tags().names(), tag);
    }

    bool listExists(MusicLibraryFixture& libraryFixture, ListId listId)
    {
      auto transaction = libraryFixture.library().readTransaction();
      return libraryFixture.library().lists().reader(transaction).get(listId).has_value();
    }

    std::size_t listCount(MusicLibraryFixture& libraryFixture)
    {
      std::size_t count = 0;
      auto transaction = libraryFixture.library().readTransaction();

      for ([[maybe_unused]] auto const& item : libraryFixture.library().lists().reader(transaction))
      {
        ++count;
      }

      return count;
    }

    std::string listName(MusicLibraryFixture& libraryFixture, ListId listId)
    {
      auto transaction = libraryFixture.library().readTransaction();
      auto const optView = libraryFixture.library().lists().reader(transaction).get(listId);
      REQUIRE(optView);
      return std::string{optView->name()};
    }

    bool listContainsTrack(MusicLibraryFixture& libraryFixture, ListId listId, TrackId trackId)
    {
      auto transaction = libraryFixture.library().readTransaction();
      auto const optView = libraryFixture.library().lists().reader(transaction).get(listId);
      REQUIRE(optView);
      return std::ranges::contains(optView->tracks(), trackId);
    }

    ListId createManualList(MusicLibraryFixture& libraryFixture,
                            std::string_view name,
                            std::vector<TrackId> trackIds = {})
    {
      auto transaction = libraryFixture.library().writeTransaction();
      auto builder = library::ListBuilder::makeEmpty().name(name);

      for (auto const trackId : trackIds)
      {
        builder.tracks().add(trackId);
      }

      auto const createResult = libraryFixture.library().lists().writer(transaction).create(builder.serialize());
      REQUIRE(createResult);
      REQUIRE(transaction.commit());
      return createResult->first;
    }

    std::filesystem::path copyFixtureAudio(MusicLibraryFixture const& libraryFixture, std::string const& name)
    {
      auto const source = std::filesystem::current_path() / "test/integration/tag/test_data/empty.flac";

      if (!std::filesystem::exists(source))
      {
        return {};
      }

      auto const destination = libraryFixture.root() / name;
      std::filesystem::create_directories(destination.parent_path());
      std::filesystem::copy_file(source, destination, std::filesystem::copy_options::overwrite_existing);
      return destination;
    }
  } // namespace

  TEST_CASE("LibraryWriter - dry-run previews metadata updates without committing", "[runtime][unit][library][dry-run]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const trackId = libraryFixture.addTrack("Before");
    auto changes = LibraryChanges{};
    auto writer = LibraryWriter{libraryFixture.library(), changes};
    auto recorder = ChangeRecorder{changes};
    auto const patch = MetadataPatch{.optTitle = "After"};

    auto const dryRun = writer.previewUpdateMetadata(std::array{trackId}, patch);

    REQUIRE(dryRun);
    REQUIRE(dryRun->changes.size() == 1);
    REQUIRE(dryRun->changes[0].fields.size() == 1);
    CHECK(dryRun->changes[0].fields[0] ==
          TrackFieldChange{.field = "title", .oldValue = "Before", .newValue = "After"});
    CHECK(trackTitle(libraryFixture, trackId) == "Before");
    CHECK(recorder.tracksMutated == 0);

    auto const commit = writer.updateMetadata(std::array{trackId}, patch);
    REQUIRE(commit);
    CHECK(*commit == *dryRun);
    CHECK(trackTitle(libraryFixture, trackId) == "After");
    CHECK(recorder.tracksMutated == 1);
  }

  TEST_CASE("LibraryWriter - dry-run previews tag edits without committing", "[runtime][unit][library][dry-run]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const trackId = libraryFixture.addTrack("Track");
    auto changes = LibraryChanges{};
    auto writer = LibraryWriter{libraryFixture.library(), changes};
    auto recorder = ChangeRecorder{changes};
    auto const tags = std::array{std::string{"Favorite"}};

    auto const dryRun = writer.previewEditTags(std::array{trackId}, tags, {});

    REQUIRE(dryRun);
    REQUIRE(dryRun->changes.size() == 1);
    CHECK(dryRun->changes[0].addedTags == std::vector<std::string>{"Favorite"});
    CHECK_FALSE(trackHasTag(libraryFixture, trackId, "Favorite"));
    CHECK(recorder.tracksMutated == 0);

    auto const commit = writer.editTags(std::array{trackId}, tags, {});
    REQUIRE(commit);
    CHECK(*commit == *dryRun);
    CHECK(trackHasTag(libraryFixture, trackId, "Favorite"));
    CHECK(recorder.tracksMutated == 1);
  }

  TEST_CASE("LibraryWriter - dry-run previews list creation without committing", "[runtime][unit][library][dry-run]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto changes = LibraryChanges{};
    auto writer = LibraryWriter{libraryFixture.library(), changes};
    auto recorder = ChangeRecorder{changes};
    auto draft = LibraryWriter::ListDraft{.kind = LibraryWriter::ListKind::Manual, .name = "Draft"};

    auto const dryRun = writer.previewCreateList(draft);

    REQUIRE(dryRun);
    CHECK(listCount(libraryFixture) == 0);
    CHECK(recorder.listsMutated == 0);

    auto const commit = writer.createList(draft);
    REQUIRE(commit);
    CHECK(listExists(libraryFixture, *commit));
    CHECK(recorder.listsMutated == 1);
  }

  TEST_CASE("LibraryWriter - dry-run previews list updates without committing", "[runtime][unit][library][dry-run]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const trackId = libraryFixture.addTrack("Track");
    auto const listId = createManualList(libraryFixture, "Before");
    auto changes = LibraryChanges{};
    auto writer = LibraryWriter{libraryFixture.library(), changes};
    auto recorder = ChangeRecorder{changes};
    auto draft = LibraryWriter::ListDraft{
      .kind = LibraryWriter::ListKind::Manual, .listId = listId, .name = "After", .trackIds = {trackId}};

    auto const dryRun = writer.previewUpdateList(draft);

    REQUIRE(dryRun);
    CHECK(dryRun->changed);
    CHECK(dryRun->fieldChanges[0] == ListFieldChange{.field = "name", .oldValue = "Before", .newValue = "After"});
    CHECK(dryRun->addedTrackIds == std::vector<TrackId>{trackId});
    CHECK(listName(libraryFixture, listId) == "Before");
    CHECK_FALSE(listContainsTrack(libraryFixture, listId, trackId));
    CHECK(recorder.listsMutated == 0);

    auto const commit = writer.updateList(draft);
    REQUIRE(commit);
    CHECK(*commit == *dryRun);
    CHECK(listName(libraryFixture, listId) == "After");
    CHECK(listContainsTrack(libraryFixture, listId, trackId));
    CHECK(recorder.listsMutated == 1);
  }

  TEST_CASE("LibraryWriter - dry-run previews list deletion without committing", "[runtime][unit][library][dry-run]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const trackId = libraryFixture.addTrack("Track");
    auto const listId = createManualList(libraryFixture, "Delete Me", {trackId});
    auto changes = LibraryChanges{};
    auto writer = LibraryWriter{libraryFixture.library(), changes};
    auto recorder = ChangeRecorder{changes};

    auto const dryRun = writer.previewDeleteList(listId);

    REQUIRE(dryRun);
    CHECK(dryRun->name == "Delete Me");
    CHECK(dryRun->kind == "manual");
    CHECK(dryRun->trackCount == 1);
    CHECK(listExists(libraryFixture, listId));
    CHECK(recorder.listsMutated == 0);

    auto const commit = writer.deleteList(listId);
    REQUIRE(commit);
    CHECK(*commit == *dryRun);
    CHECK_FALSE(listExists(libraryFixture, listId));
    CHECK(recorder.listsMutated == 1);
  }

  TEST_CASE("LibraryWriter - dry-run previews track deletion without committing", "[runtime][unit][library][dry-run]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const trackId = libraryFixture.addTrack("Delete Track");
    auto const listId = createManualList(libraryFixture, "Manual", {trackId});
    auto changes = LibraryChanges{};
    auto writer = LibraryWriter{libraryFixture.library(), changes};
    auto recorder = ChangeRecorder{changes};

    auto const dryRun = writer.previewDeleteTrack(trackId);

    REQUIRE(dryRun);
    CHECK(dryRun->trackId == trackId);
    CHECK(dryRun->title == "Delete Track");
    CHECK(dryRun->removedFromListIds == std::vector<ListId>{listId});
    CHECK(trackExists(libraryFixture, trackId));
    CHECK(listContainsTrack(libraryFixture, listId, trackId));
    CHECK(recorder.tracksMutated == 0);
    CHECK(recorder.collectionChanged == 0);
    CHECK(recorder.listsMutated == 0);

    auto const commit = writer.deleteTrack(trackId);
    REQUIRE(commit);
    CHECK(*commit == *dryRun);
    CHECK_FALSE(trackExists(libraryFixture, trackId));
    CHECK_FALSE(listContainsTrack(libraryFixture, listId, trackId));
    CHECK(recorder.tracksMutated == 1);
    CHECK(recorder.collectionChanged == 1);
    CHECK(recorder.listsMutated == 1);
  }

  TEST_CASE("LibraryWriter - dry-run previews track creation without committing", "[runtime][unit][library][dry-run]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto changes = LibraryChanges{};
    auto writer = LibraryWriter{libraryFixture.library(), changes};
    auto recorder = ChangeRecorder{changes};
    auto const absValidFile = copyFixtureAudio(libraryFixture, "music/song.flac");

    if (!std::filesystem::exists(absValidFile))
    {
      SUCCEED("Skipping test because test file is missing");
      return;
    }

    auto const dryRun = writer.previewCreateTrackFromFile(absValidFile);

    REQUIRE(dryRun);
    CHECK(dryRun->uri == "music/song.flac");
    CHECK(trackCount(libraryFixture) == 0);
    CHECK_FALSE(
      libraryFixture.library().manifest().reader(libraryFixture.library().readTransaction()).get("music/song.flac"));
    CHECK(recorder.tracksMutated == 0);
    CHECK(recorder.collectionChanged == 0);

    auto const commit = writer.createTrackFromFile(absValidFile);
    REQUIRE(commit);
    CHECK(commit->uri == dryRun->uri);
    CHECK(commit->title == dryRun->title);
    CHECK(commit->artist == dryRun->artist);
    CHECK(trackExists(libraryFixture, commit->trackId));
    CHECK(
      libraryFixture.library().manifest().reader(libraryFixture.library().readTransaction()).get("music/song.flac"));
    CHECK(recorder.tracksMutated == 1);
    CHECK(recorder.collectionChanged == 1);
  }
} // namespace ao::rt::test
