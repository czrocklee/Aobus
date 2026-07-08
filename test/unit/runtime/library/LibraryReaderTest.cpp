// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/RuntimeTestUtils.h"
#include "test/unit/TestUtils.h"
#include <ao/AudioCodec.h>
#include <ao/AudioScalars.h>
#include <ao/CoreIds.h>
#include <ao/library/CoverArt.h>
#include <ao/library/FileManifestBuilder.h>
#include <ao/library/FileManifestLayout.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/ListBuilder.h>
#include <ao/library/ListStore.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/ResourceStore.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackStore.h>
#include <ao/rt/CoreRuntime.h>
#include <ao/rt/ListNode.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackRow.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryReader.h>

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
      ListId manualListId{};
      ListId smartListId{};
    };

    CoreRuntime makeCoreRuntime(ao::test::TempDir const& tempDir)
    {
      return CoreRuntime{
        std::make_unique<MockExecutor>(), tempDir.path(), std::filesystem::path{tempDir.path()} / ".aobus" / "library"};
    }

    SeededReadModelLibrary seedLibrary(CoreRuntime& runtime)
    {
      auto& library = runtime.musicLibrary();
      auto txn = library.writeTransaction();

      auto resourceWriter = library.resources().writer(txn);
      auto resourceIdResult = resourceWriter.create(kCoverBytes);
      REQUIRE(resourceIdResult);
      auto const resourceId = *resourceIdResult;

      auto trackBuilder = library::TrackBuilder::createNew();
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
        .duration(245s) // NOLINT(misc-include-cleaner)
        .bitrate(Bitrate{960000})
        .sampleRate(SampleRate{48000})
        .channels(Channels{2})
        .bitDepth(BitDepth{24})
        .codec(AudioCodec::Flac);
      trackBuilder.tags().add("Favorite").add("Live");
      trackBuilder.coverArt().add(library::PictureType::FrontCover, resourceId);

      auto hotData = trackBuilder.serializeHot(txn, library.dictionary());
      REQUIRE(hotData);
      auto coldData = trackBuilder.serializeCold(txn, library.dictionary(), library.resources());
      REQUIRE(coldData);
      auto trackWriter = library.tracks().writer(txn);
      [[maybe_unused]] auto [trackId, trackView] =
        ao::test::requireValue(trackWriter.createHotCold(*hotData, *coldData));

      auto otherTrackBuilder = library::TrackBuilder::createNew();
      otherTrackBuilder.metadata().title("Another Song");
      otherTrackBuilder.tags().add("Favorite").add("Jazz");
      auto otherSerializeResult = otherTrackBuilder.serialize(txn, library.dictionary(), library.resources());
      REQUIRE(otherSerializeResult);
      auto const [otherHot, otherCold] = *otherSerializeResult;
      [[maybe_unused]] auto [otherTrackId, otherTrackView] =
        ao::test::requireValue(trackWriter.createHotCold(otherHot, otherCold));

      auto manifestPayload = library::FileManifestBuilder::createNew()
                               .trackId(trackId)
                               .fileSize(123456789)
                               .mtime(987654321)
                               .status(library::FileStatus::Missing)
                               .serialize();
      CHECK(library.manifest().writer(txn).put(kTrackUri, manifestPayload));

      auto manualListBuilder = library::ListBuilder::createNew();
      manualListBuilder.name("Manual List").description("Pinned songs").tracks().add(trackId);
      [[maybe_unused]] auto [manualListId, manualListView] =
        ao::test::requireValue(library.lists().writer(txn).create(manualListBuilder.serialize()));

      auto smartListBuilder = library::ListBuilder::createNew();
      smartListBuilder.name("Smart List").parentId(manualListId).filter("@artist = \"An Artist\"");
      [[maybe_unused]] auto [smartListId, smartListView] =
        ao::test::requireValue(library.lists().writer(txn).create(smartListBuilder.serialize()));

      auto const artistId = library.dictionary().getId("An Artist");
      auto const albumId = library.dictionary().getId("The Album");

      REQUIRE(txn.commit());

      return SeededReadModelLibrary{.trackId = trackId,
                                    .otherTrackId = otherTrackId,
                                    .resourceId = resourceId,
                                    .artistId = artistId,
                                    .albumId = albumId,
                                    .manualListId = manualListId,
                                    .smartListId = smartListId};
    }
  } // namespace

  TEST_CASE("LibraryReader - reads track, dictionary, and resource DTOs", "[runtime][unit][library][readmodel]")
  {
    auto tempDir = ao::test::TempDir{};
    auto runtime = makeCoreRuntime(tempDir);
    auto const seeded = seedLibrary(runtime);
    auto const& reads = runtime.library();

    auto scope = reads.reader();
    REQUIRE(scope.valid());

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
    CHECK(row.duration == 245s); // NOLINT(misc-include-cleaner)
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
    REQUIRE(scope.trackUriPath(seeded.trackId).has_value());
    CHECK(*scope.trackUriPath(seeded.trackId) ==
          (std::filesystem::path{tempDir.path()} / kTrackUri).lexically_normal());

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
    CHECK_FALSE(scope.trackUriPath(TrackId{999999}).has_value());

    CHECK(scope.resolve(seeded.artistId) == "An Artist");
    auto const dictionaryIds = std::array{seeded.artistId, seeded.albumId};
    auto const resolved = scope.resolveAll(dictionaryIds);
    CHECK(resolved == std::vector<std::string>{"An Artist", "The Album"});

    auto const optResource = scope.loadResource(seeded.resourceId);
    REQUIRE(optResource);
    CHECK(*optResource == std::vector<std::byte>{kCoverBytes.begin(), kCoverBytes.end()});
    CHECK_FALSE(scope.loadResource(ResourceId{999999}).has_value());
  }

  TEST_CASE("LibraryReader - snapshots list tree DTOs", "[runtime][unit][library][readmodel]")
  {
    auto tempDir = ao::test::TempDir{};
    auto runtime = makeCoreRuntime(tempDir);
    auto const seeded = seedLibrary(runtime);

    auto scope = runtime.library().reader();
    auto const nodes = scope.lists();

    auto const manualIt =
      std::ranges::find_if(nodes, [&](ListNode const& node) { return node.id == seeded.manualListId; });
    REQUIRE(manualIt != nodes.end());
    CHECK(manualIt->parentId == kInvalidListId);
    CHECK(manualIt->name == "Manual List");
    CHECK(manualIt->description == "Pinned songs");
    CHECK(manualIt->kind == ListNodeKind::Manual);
    CHECK(manualIt->smartExpression.empty());

    auto const optManualNode = scope.listNode(seeded.manualListId);
    REQUIRE(optManualNode);
    CHECK(optManualNode->name == "Manual List");
    CHECK(optManualNode->description == "Pinned songs");

    auto const smartIt =
      std::ranges::find_if(nodes, [&](ListNode const& node) { return node.id == seeded.smartListId; });
    REQUIRE(smartIt != nodes.end());
    CHECK(smartIt->parentId != kInvalidListId);
    CHECK(smartIt->parentId == seeded.manualListId);
    CHECK(smartIt->name == "Smart List");
    CHECK(smartIt->kind == ListNodeKind::Smart);
    CHECK(smartIt->smartExpression == "@artist = \"An Artist\"");

    auto const optMissingNode = scope.listNode(ListId{999999});
    CHECK_FALSE(optMissingNode);
  }

  TEST_CASE("LibraryReader - reads stored list membership", "[runtime][unit][library][readmodel]")
  {
    auto tempDir = ao::test::TempDir{};
    auto runtime = makeCoreRuntime(tempDir);
    auto const seeded = seedLibrary(runtime);

    auto scope = runtime.library().reader();

    CHECK(scope.listTrackIds(seeded.manualListId) == std::vector<TrackId>{seeded.trackId});
    CHECK(scope.listTrackIds(seeded.smartListId).empty());
    CHECK(scope.listTrackIds(ListId{999999}).empty());
  }

  TEST_CASE("LibraryReader - snapshots tag DTOs", "[runtime][unit][library][readmodel]")
  {
    auto tempDir = ao::test::TempDir{};
    auto runtime = makeCoreRuntime(tempDir);
    auto const seeded = seedLibrary(runtime);
    auto const& reads = runtime.library();

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
