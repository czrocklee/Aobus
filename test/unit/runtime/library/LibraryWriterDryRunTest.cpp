// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/library/WritableLibraryTestSupport.h"
#include "test/unit/runtime/RuntimeLibraryTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/async/Subscription.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/LibraryWrite.h>
#include <ao/library/ListBuilder.h>
#include <ao/library/ListStore.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackStore.h>
#include <ao/rt/ListMutation.h>
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
        : tracksSub{changes.onChanged([this](LibraryChangeSet const& event) noexcept
                                      { tracksMutated += !event.tracksMutated.empty(); })}
        , collectionSub{changes.onChanged(
            [this](LibraryChangeSet const& event) noexcept
            { collectionChanged += !event.tracksInserted.empty() || !event.tracksDeleted.empty(); })}
        , listsSub{changes.onChanged([this](LibraryChangeSet const& event) noexcept
                                     { listsMutated += !event.listsUpserted.empty() || !event.listsDeleted.empty(); })}
      {
      }

      std::int32_t tracksMutated = 0;
      std::int32_t collectionChanged = 0;
      std::int32_t listsMutated = 0;

      async::Subscription tracksSub;
      async::Subscription collectionSub;
      async::Subscription listsSub;
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
      auto builder = library::TrackBuilder::fromHotView(*optView, libraryFixture.library().dictionary());
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

    bool listOrderContainsTrack(MusicLibraryFixture& libraryFixture, ListId listId, TrackId trackId)
    {
      auto transaction = libraryFixture.library().readTransaction();
      auto const optView = libraryFixture.library().lists().reader(transaction).get(listId);
      REQUIRE(optView);
      return std::ranges::contains(optView->orderTrackIds(), trackId);
    }

    ListId createListWithOrder(MusicLibraryFixture& libraryFixture,
                               std::string_view name,
                               std::vector<TrackId> trackIds = {})
    {
      auto transaction = library::test::writeTransaction(libraryFixture.library());
      auto builder = library::ListBuilder::makeEmpty().name(name);

      for (auto const trackId : trackIds)
      {
        builder.orderTrackIds().add(trackId);
      }

      auto const createRes =
        transaction.apply([&builder](library::LibraryWrite& write) { return write.lists().create(builder); });
      REQUIRE(createRes);
      REQUIRE(transaction.commit());
      return *createRes;
    }

    std::filesystem::path copyFixtureAudio(MusicLibraryFixture const& libraryFixture, std::string const& name)
    {
      auto const source = std::filesystem::path{AUDIO_TEST_DATA_DIR} / "empty.flac";

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
    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto writerFixture = LibraryWriterFixture{libraryFixture.library(), changes};
    auto& writer = writerFixture.writer();
    auto recorder = ChangeRecorder{changes};
    auto const patch = MetadataPatch{.optTitle = "After"};

    auto const dryRunRes = writerFixture.runTask(writer.previewUpdateMetadata(std::vector{trackId}, patch));

    REQUIRE(dryRunRes);
    REQUIRE(dryRunRes->changes.size() == 1);
    REQUIRE(dryRunRes->changes[0].fields.size() == 1);
    CHECK(dryRunRes->changes[0].fields[0] ==
          TrackFieldChange{.field = "title", .oldValue = "Before", .newValue = "After"});
    CHECK(trackTitle(libraryFixture, trackId) == "Before");
    CHECK(recorder.tracksMutated == 0);

    auto const commitRes = writerFixture.updateMetadata(std::array{trackId}, patch);
    REQUIRE(commitRes);
    CHECK(*commitRes == *dryRunRes);
    CHECK(trackTitle(libraryFixture, trackId) == "After");
    CHECK(recorder.tracksMutated == 1);
  }

  TEST_CASE("LibraryWriter - metadata preview does not publish dictionary symbols", "[runtime][unit][library][dry-run]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const trackId = libraryFixture.addTrack("Track");
    auto const& dictionary = libraryFixture.library().dictionary();
    auto const initialSize = dictionary.size();
    auto const initialGeneration = dictionary.generation();
    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto writerFixture = LibraryWriterFixture{libraryFixture.library(), changes};
    auto& writer = writerFixture.writer();
    bool callbackSawDictionary = false;
    [[maybe_unused]] auto subscription = changes.onChanged(
      [&](LibraryChangeSet const&) noexcept
      { callbackSawDictionary = dictionary.findId("Preview Artist") && dictionary.findId("Preview Key"); });
    auto patch = MetadataPatch{.optArtist = "Preview Artist"};
    patch.customUpdates.emplace("Preview Key", "Preview Value");

    auto const previewRes = writerFixture.runTask(writer.previewUpdateMetadata(std::vector{trackId}, patch));

    REQUIRE(previewRes);
    CHECK(dictionary.size() == initialSize);
    CHECK(dictionary.generation() == initialGeneration);
    CHECK_FALSE(dictionary.findId("Preview Artist"));
    CHECK_FALSE(dictionary.findId("Preview Key"));
    CHECK_FALSE(callbackSawDictionary);

    auto const commitRes = writerFixture.updateMetadata(std::array{trackId}, patch);

    REQUIRE(commitRes);
    CHECK(*commitRes == *previewRes);
    CHECK(dictionary.size() == initialSize + 2);
    CHECK(dictionary.generation() == initialGeneration + 1);
    CHECK(callbackSawDictionary);
  }

  TEST_CASE("LibraryWriter - dry-run previews tag edits without committing", "[runtime][unit][library][dry-run]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const trackId = libraryFixture.addTrack("Track");
    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto writerFixture = LibraryWriterFixture{libraryFixture.library(), changes};
    auto& writer = writerFixture.writer();
    auto recorder = ChangeRecorder{changes};
    auto const tags = std::array{std::string{"Favorite"}};
    auto const& dictionary = libraryFixture.library().dictionary();
    auto const initialSize = dictionary.size();
    auto const initialGeneration = dictionary.generation();

    auto const dryRunRes =
      writerFixture.runTask(writer.previewEditTags(std::vector{trackId}, {tags.begin(), tags.end()}, {}));

    REQUIRE(dryRunRes);
    REQUIRE(dryRunRes->changes.size() == 1);
    CHECK(dryRunRes->changes[0].addedTags == std::vector<std::string>{"Favorite"});
    CHECK_FALSE(trackHasTag(libraryFixture, trackId, "Favorite"));
    CHECK_FALSE(dictionary.findId("Favorite"));
    CHECK(dictionary.size() == initialSize);
    CHECK(dictionary.generation() == initialGeneration);
    CHECK(recorder.tracksMutated == 0);

    auto const commitRes = writerFixture.editTags(std::array{trackId}, tags, {});
    REQUIRE(commitRes);
    CHECK(*commitRes == *dryRunRes);
    CHECK(trackHasTag(libraryFixture, trackId, "Favorite"));
    CHECK(dictionary.findId("Favorite"));
    CHECK(dictionary.size() == initialSize + 1);
    CHECK(dictionary.generation() == initialGeneration + 1);
    CHECK(recorder.tracksMutated == 1);
  }

  TEST_CASE("LibraryWriter - dry-run previews list creation without committing", "[runtime][unit][library][dry-run]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto writerFixture = LibraryWriterFixture{libraryFixture.library(), changes};
    auto& writer = writerFixture.writer();
    auto recorder = ChangeRecorder{changes};
    auto draft = ListDraft{.name = "Draft"};

    auto const dryRunRes = writerFixture.runTask(writer.previewCreateList(draft));

    REQUIRE(dryRunRes);
    CHECK(listCount(libraryFixture) == 0);
    CHECK(recorder.listsMutated == 0);

    auto const commitRes = writerFixture.runTask(writer.createList(draft));
    REQUIRE(commitRes);
    CHECK(listExists(libraryFixture, *commitRes));
    CHECK(recorder.listsMutated == 1);
  }

  TEST_CASE("LibraryWriter - dry-run previews list updates without committing", "[runtime][unit][library][dry-run]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const trackId = libraryFixture.addTrack("Track");
    auto const listId = createListWithOrder(libraryFixture, "Before", {trackId});
    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto writerFixture = LibraryWriterFixture{libraryFixture.library(), changes};
    auto& writer = writerFixture.writer();
    auto recorder = ChangeRecorder{changes};
    auto draft = ListDraft{.listId = listId, .name = "After"};

    auto const dryRunRes = writerFixture.runTask(writer.previewUpdateList(draft));

    REQUIRE(dryRunRes);
    CHECK(dryRunRes->changed);
    CHECK(dryRunRes->fieldChanges[0] == ListFieldChange{.field = "name", .oldValue = "Before", .newValue = "After"});
    CHECK(listName(libraryFixture, listId) == "Before");
    CHECK(listOrderContainsTrack(libraryFixture, listId, trackId));
    CHECK(recorder.listsMutated == 0);

    auto const commitRes = writerFixture.runTask(writer.updateList(draft));
    REQUIRE(commitRes);
    CHECK(*commitRes == *dryRunRes);
    CHECK(listName(libraryFixture, listId) == "After");
    CHECK(listOrderContainsTrack(libraryFixture, listId, trackId));
    CHECK(recorder.listsMutated == 1);
  }

  TEST_CASE("LibraryWriter - dry-run previews list deletion without committing", "[runtime][unit][library][dry-run]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const trackId = libraryFixture.addTrack("Track");
    auto const listId = createListWithOrder(libraryFixture, "Delete Me", {trackId});
    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto writerFixture = LibraryWriterFixture{libraryFixture.library(), changes};
    auto& writer = writerFixture.writer();
    auto recorder = ChangeRecorder{changes};

    auto const dryRunRes = writerFixture.runTask(writer.previewDeleteList(listId));

    REQUIRE(dryRunRes);
    CHECK(dryRunRes->name == "Delete Me");
    CHECK(dryRunRes->orderTrackIdCount == 1);
    CHECK(listExists(libraryFixture, listId));
    CHECK(recorder.listsMutated == 0);

    auto const commitRes = writerFixture.runTask(writer.deleteList(listId));
    REQUIRE(commitRes);
    CHECK(*commitRes == *dryRunRes);
    CHECK_FALSE(listExists(libraryFixture, listId));
    CHECK(recorder.listsMutated == 1);
  }

  TEST_CASE("LibraryWriter - dry-run previews track deletion without committing", "[runtime][unit][library][dry-run]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const trackId = libraryFixture.addTrack("Delete Track");
    auto const listId = createListWithOrder(libraryFixture, "Ordered", {trackId});
    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto writerFixture = LibraryWriterFixture{libraryFixture.library(), changes};
    auto& writer = writerFixture.writer();
    auto recorder = ChangeRecorder{changes};

    auto const dryRunRes = writerFixture.runTask(writer.previewDeleteTrack(trackId));

    REQUIRE(dryRunRes);
    CHECK(dryRunRes->trackId == trackId);
    CHECK(dryRunRes->title == "Delete Track");
    CHECK(dryRunRes->removedFromListIds == std::vector<ListId>{listId});
    CHECK(trackExists(libraryFixture, trackId));
    CHECK(listOrderContainsTrack(libraryFixture, listId, trackId));
    CHECK(recorder.tracksMutated == 0);
    CHECK(recorder.collectionChanged == 0);
    CHECK(recorder.listsMutated == 0);

    auto const commitRes = writerFixture.runTask(writer.deleteTrack(trackId));
    REQUIRE(commitRes);
    CHECK(*commitRes == *dryRunRes);
    CHECK_FALSE(trackExists(libraryFixture, trackId));
    CHECK_FALSE(listOrderContainsTrack(libraryFixture, listId, trackId));
    CHECK(recorder.tracksMutated == 0);
    CHECK(recorder.collectionChanged == 1);
    CHECK(recorder.listsMutated == 1);
  }

  TEST_CASE("LibraryWriter - dry-run previews track creation without committing", "[runtime][unit][library][dry-run]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto writerFixture = LibraryWriterFixture{libraryFixture.library(), changes};
    auto& writer = writerFixture.writer();
    auto recorder = ChangeRecorder{changes};
    auto const absValidFile = copyFixtureAudio(libraryFixture, "music/song.flac");

    if (!std::filesystem::exists(absValidFile))
    {
      SUCCEED("Skipping test because test file is missing");
      return;
    }

    auto const dryRunRes = writerFixture.runTask(writer.previewCreateTrackFromFile(absValidFile));

    REQUIRE(dryRunRes);
    CHECK(dryRunRes->uri == "music/song.flac");
    CHECK(trackCount(libraryFixture) == 0);
    CHECK_FALSE(
      libraryFixture.library().manifest().reader(libraryFixture.library().readTransaction()).get("music/song.flac"));
    CHECK(recorder.tracksMutated == 0);
    CHECK(recorder.collectionChanged == 0);

    auto const commitRes = writerFixture.runTask(writer.createTrackFromFile(absValidFile));
    REQUIRE(commitRes);
    CHECK(commitRes->uri == dryRunRes->uri);
    CHECK(commitRes->title == dryRunRes->title);
    CHECK(commitRes->artist == dryRunRes->artist);
    CHECK(trackExists(libraryFixture, commitRes->trackId));
    CHECK(
      libraryFixture.library().manifest().reader(libraryFixture.library().readTransaction()).get("music/song.flac"));
    CHECK(recorder.tracksMutated == 0);
    CHECK(recorder.collectionChanged == 1);
  }
} // namespace ao::rt::test
