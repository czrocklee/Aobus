// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/library/LibraryReader.h>

#include "test/unit/FilesystemTestSupport.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/library/MusicLibraryTestSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/library/WritableLibraryTestSupport.h"
#include "test/unit/runtime/ExecutorTestSupport.h"
#include "test/unit/runtime/RuntimeLibraryTestSupport.h"
#include <ao/AudioCodec.h>
#include <ao/AudioScalars.h>
#include <ao/CoreIds.h>
#include <ao/PictureType.h>
#include <ao/async/Runtime.h>
#include <ao/library/FileManifestBuilder.h>
#include <ao/library/FileManifestLayout.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/ListBuilder.h>
#include <ao/library/ListStore.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/ResourceStore.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackStore.h>
#include <ao/library/TrackWrite.h>
#include <ao/rt/CoreRuntime.h>
#include <ao/rt/ListNode.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackRow.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryPaths.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace ao::rt::test
{
  using namespace ao::test;
  using namespace std::chrono_literals;

  namespace
  {
    constexpr auto kTrackUri = "music/song.flac";
    constexpr auto kCoverBytes = std::array{std::byte{0x01}, std::byte{0x23}, std::byte{0x45}, std::byte{0x67}};

    struct SeededReadModelLibrary final
    {
      TrackId trackId{};
      TrackId otherTrackId{};
      ResourceId resourceId{};
      DictionaryId artistId{};
      DictionaryId albumId{};
      ListId orderedListId{};
      ListId filteredListId{};
    };

    std::unique_ptr<CoreRuntime> makeCoreRuntime(ao::test::TempDir const& tempDir)
    {
      return ao::test::requireValue(CoreRuntime::create(std::make_unique<InlineExecutor>(),
                                                        tempDir.path(),
                                                        LibraryPaths{tempDir.path()}.databasePath(),
                                                        library::test::kTestMusicLibraryMapSize));
    }

    SeededReadModelLibrary seedLibrary(ao::test::TempDir const& tempDir)
    {
      auto musicLibrary =
        library::test::makeTestMusicLibrary(tempDir.path(), LibraryPaths{tempDir.path()}.databasePath());
      auto transaction = library::test::writeTransaction(musicLibrary);

      auto resourceWriter = musicLibrary.resources().writer(transaction);
      auto resourceIdResult = resourceWriter.create(kCoverBytes);
      REQUIRE(resourceIdResult);
      auto const resourceId = *resourceIdResult;

      auto trackBuilder = library::TrackBuilder::makeEmpty();
      trackBuilder.metadata()
        .title("A Song")
        .artist("An Artist")
        .album("The Album")
        .albumArtist("Album Artist")
        .genre("Rock")
        .composer("Composer")
        .conductor("Conductor")
        .ensemble("Ensemble")
        .work("Work")
        .movement("Movement")
        .soloist("Soloist")
        .year(2026)
        .discNumber(2)
        .discTotal(3)
        .trackNumber(4)
        .trackTotal(12)
        .movementNumber(1)
        .movementTotal(2);
      trackBuilder.property()
        .uri(kTrackUri)
        // <chrono> owns this literal; include-cleaner cannot map the using-directive provider.
        .duration(245s) // NOLINT(misc-include-cleaner)
        .bitrate(Bitrate{960000})
        .sampleRate(SampleRate{48000})
        .channels(Channels{2})
        .bitDepth(BitDepth{24})
        .codec(AudioCodec::Flac);
      trackBuilder.tags().add("Favorite").add("Live");
      trackBuilder.coverArt().add(PictureType::FrontCover, resourceId);

      auto prepared = trackBuilder.prepare(transaction, musicLibrary.resources());
      REQUIRE(prepared);
      auto trackWriter = musicLibrary.tracks().writer(transaction);
      auto const trackId =
        ao::test::requireValue(library::createPreparedTrackRecord(trackWriter, prepared->first, prepared->second));

      auto otherTrackBuilder = library::TrackBuilder::makeEmpty();
      otherTrackBuilder.metadata().title("Another Song");
      otherTrackBuilder.property().uri("other.flac");
      otherTrackBuilder.tags().add("Favorite").add("Jazz");
      auto otherPrepared = otherTrackBuilder.prepare(transaction, musicLibrary.resources());
      REQUIRE(otherPrepared);
      auto const otherTrackId = ao::test::requireValue(
        library::createPreparedTrackRecord(trackWriter, otherPrepared->first, otherPrepared->second));

      auto manifestPayload = library::FileManifestBuilder::makeEmpty()
                               .trackId(trackId)
                               .fileSize(123456789)
                               .mtime(987654321)
                               .status(library::FileStatus::Missing)
                               .serialize();
      CHECK(musicLibrary.manifest().writer(transaction).put(kTrackUri, manifestPayload));

      auto orderedListBuilder = library::ListBuilder::makeEmpty();
      orderedListBuilder.name("Ordered List").description("Pinned songs").orderTrackIds().add(trackId);
      auto const orderedListId = ao::test::requireValue(
        musicLibrary.lists().writer(transaction).create(ao::test::requireValue(orderedListBuilder.serialize())));

      auto filteredListBuilder = library::ListBuilder::makeEmpty();
      filteredListBuilder.name("Filtered List").parentId(orderedListId).filter("@artist = \"An Artist\"");
      auto const filteredListId = ao::test::requireValue(
        musicLibrary.lists().writer(transaction).create(ao::test::requireValue(filteredListBuilder.serialize())));

      REQUIRE(transaction.commit());
      auto const artistId = musicLibrary.dictionary().lookupId("An Artist");
      auto const albumId = musicLibrary.dictionary().lookupId("The Album");

      return SeededReadModelLibrary{.trackId = trackId,
                                    .otherTrackId = otherTrackId,
                                    .resourceId = resourceId,
                                    .artistId = artistId,
                                    .albumId = albumId,
                                    .orderedListId = orderedListId,
                                    .filteredListId = filteredListId};
    }
  } // namespace

  TEST_CASE("LibraryReader - reads track rows and dictionary values", "[runtime][unit][library][readmodel]")
  {
    auto tempDir = ao::test::TempDir{};
    auto const seeded = seedLibrary(tempDir);
    auto runtimePtr = makeCoreRuntime(tempDir);
    auto const& reads = runtimePtr->library();

    auto scope = reads.reader();

    auto const optRow = scope.trackRow(seeded.trackId);
    REQUIRE(optRow);

    auto const& row = *optRow;
    CHECK(row.id == seeded.trackId);
    CHECK(row.coverArtId == seeded.resourceId);
    REQUIRE(row.optUriPath);
    CHECK(*row.optUriPath == (std::filesystem::path{tempDir.path()} / kTrackUri).lexically_normal());
    CHECK(row.title == "A Song");
    CHECK(row.artist == "An Artist");
    CHECK(row.album == "The Album");
    CHECK(row.albumArtist == "Album Artist");
    CHECK(row.genre == "Rock");
    CHECK(row.composer == "Composer");
    CHECK(row.conductor == "Conductor");
    CHECK(row.ensemble == "Ensemble");
    CHECK(row.work == "Work");
    CHECK(row.movement == "Movement");
    CHECK(row.soloist == "Soloist");
    CHECK(row.tags == "Favorite, Live");
    CHECK(row.duration == 245s);
    CHECK(row.year == 2026);
    CHECK(row.discNumber == 2);
    CHECK(row.discTotal == 3);
    CHECK(row.trackNumber == 4);
    CHECK(row.trackTotal == 12);
    CHECK(row.movementNumber == 1);
    CHECK(row.movementTotal == 2);
    CHECK(row.sampleRate == 48000);
    CHECK(row.channels == 2);
    CHECK(row.bitDepth == 24);
    CHECK(row.codec == AudioCodec::Flac);
    CHECK(row.bitrate == 960000);
    CHECK(row.fileSize == 123456789);
    CHECK(row.modifiedTime == 987654321);
    CHECK(row.status == library::FileStatus::Missing);

    CHECK(scope.trackCoverArtId(seeded.trackId) == seeded.resourceId);

    auto const title = scope.trackField(seeded.trackId, TrackField::Title);
    REQUIRE(std::holds_alternative<std::string>(title));
    CHECK(std::get<std::string>(title) == "A Song");

    auto const conductor = scope.trackField(seeded.trackId, TrackField::Conductor);
    REQUIRE(std::holds_alternative<std::string>(conductor));
    CHECK(std::get<std::string>(conductor) == "Conductor");

    auto const fileSize = scope.trackField(seeded.trackId, TrackField::FileSize);
    REQUIRE(std::holds_alternative<std::uint64_t>(fileSize));
    CHECK(std::get<std::uint64_t>(fileSize) == 123456789);

    auto const missingField = scope.trackField(TrackId{999999}, TrackField::Title);
    CHECK(std::holds_alternative<std::monostate>(missingField));
    CHECK_FALSE(scope.trackRow(TrackId{999999}).has_value());
    CHECK(scope.trackCoverArtId(TrackId{999999}) == kInvalidResourceId);

    CHECK(scope.resolve(seeded.artistId) == "An Artist");
    CHECK(scope.resolve(seeded.albumId) == "The Album");
  }

  TEST_CASE("LibraryReader - URI paths reject symlinks escaping the library root",
            "[runtime][unit][library][readmodel]")
  {
    auto const temp = ao::test::TempDir{};
    auto const musicRoot = temp.path() / "music";
    auto const outsideRoot = temp.path() / "outside";
    std::filesystem::create_directories(musicRoot);
    std::filesystem::create_directories(outsideRoot);
    auto const symlink = ao::test::SymlinkFixture{outsideRoot, musicRoot / "alias", ao::test::SymlinkType::Directory};
    auto ml = library::test::makeTestMusicLibrary(musicRoot, temp.path() / "db");
    auto const trackId = library::test::addTrack(ml, library::test::makeEmptyTrackSpec("alias/song.flac"));

    auto executor = InlineExecutor{};
    auto asyncRuntime = async::Runtime{executor};
    auto changes = makeStateOnlyLibraryChanges(ml);
    auto runtimeLibraryPtr = ao::test::requireValue(Library::create(asyncRuntime, ml, changes));
    auto reader = runtimeLibraryPtr->reader();
    auto const optRow = reader.trackRow(trackId);
    REQUIRE(optRow);
    CHECK_FALSE(optRow->optUriPath);
  }

  TEST_CASE("LibraryReader - snapshots list tree DTOs", "[runtime][unit][library][readmodel]")
  {
    auto tempDir = ao::test::TempDir{};
    auto const seeded = seedLibrary(tempDir);
    auto runtimePtr = makeCoreRuntime(tempDir);

    auto scope = runtimePtr->library().reader();
    auto const nodes = scope.lists();

    auto const orderedIt =
      std::ranges::find_if(nodes, [&](ListNode const& node) { return node.id == seeded.orderedListId; });
    REQUIRE(orderedIt != nodes.end());
    CHECK(orderedIt->parentId == kInvalidListId);
    CHECK(orderedIt->name == "Ordered List");
    CHECK(orderedIt->description == "Pinned songs");
    CHECK(orderedIt->expression.empty());

    auto const optOrderedNode = scope.listNode(seeded.orderedListId);
    REQUIRE(optOrderedNode);
    CHECK(optOrderedNode->name == "Ordered List");
    CHECK(optOrderedNode->description == "Pinned songs");

    auto const filteredIt =
      std::ranges::find_if(nodes, [&](ListNode const& node) { return node.id == seeded.filteredListId; });
    REQUIRE(filteredIt != nodes.end());
    CHECK(filteredIt->parentId != kInvalidListId);
    CHECK(filteredIt->parentId == seeded.orderedListId);
    CHECK(filteredIt->name == "Filtered List");
    CHECK(filteredIt->expression == "@artist = \"An Artist\"");

    auto const optMissingNode = scope.listNode(ListId{999999});
    CHECK_FALSE(optMissingNode);
  }

  TEST_CASE("LibraryReader - reads stored List order", "[runtime][unit][library][readmodel]")
  {
    auto tempDir = ao::test::TempDir{};
    auto const seeded = seedLibrary(tempDir);
    auto runtimePtr = makeCoreRuntime(tempDir);

    auto scope = runtimePtr->library().reader();

    CHECK(scope.listOrderTrackIds(seeded.orderedListId) == std::vector<TrackId>{seeded.trackId});
    CHECK(scope.listOrderTrackIds(seeded.filteredListId).empty());
    CHECK(scope.listOrderTrackIds(ListId{999999}).empty());
  }

  TEST_CASE("LibraryReader - snapshots tag DTOs", "[runtime][unit][library][readmodel]")
  {
    auto tempDir = ao::test::TempDir{};
    auto const seeded = seedLibrary(tempDir);
    auto runtimePtr = makeCoreRuntime(tempDir);
    auto const& reads = runtimePtr->library();

    auto scope = reads.reader();
    auto const selectedIds = std::array{seeded.trackId, seeded.otherTrackId};

    // Only "Favorite" is shared by both selected tracks.
    CHECK(scope.selectionTags(selectedIds) == std::vector<std::string>{"Favorite"});

    auto const byFrequency = scope.allTagsByFrequency();
    REQUIRE(byFrequency.size() >= 3);
    auto const firstThree =
      std::vector<std::pair<std::string, std::size_t>>{byFrequency.begin(), byFrequency.begin() + 3};
    CHECK(firstThree == std::vector<std::pair<std::string, std::size_t>>{{"Favorite", 2}, {"Jazz", 1}, {"Live", 1}});

    // A stale id in the selection contributes no tags, collapsing the intersection.
    auto const selectionWithMissing = std::array{seeded.trackId, TrackId{999999}};
    CHECK(scope.selectionTags(selectionWithMissing).empty());

    CHECK(scope.selectionTags(std::span<TrackId const>{}).empty());
  }
} // namespace ao::rt::test
