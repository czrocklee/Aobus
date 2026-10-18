// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "lib/library/TrackRecordValidation.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/library/MusicLibraryTestSupport.h"
#include "test/unit/library/TrackStoreTestSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/library/WritableLibraryTestSupport.h"
#include "test/unit/lmdb/LmdbTestSupport.h"
#include <ao/AudioScalars.h>
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/DictionaryStore.h>
#include <ao/library/FileManifestBuilder.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/LibraryUri.h>
#include <ao/library/LibraryWrite.h>
#include <ao/library/ListBuilder.h>
#include <ao/library/ListStore.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackLayout.h>
#include <ao/library/TrackStore.h>
#include <ao/library/TrackView.h>
#include <ao/library/TrackWriter.h>
#include <ao/library/WritableMusicLibrary.h>
#include <ao/library/WriteTransaction.h>
#include <ao/library/detail/TrackColdReader.h>
#include <ao/lmdb/Environment.h>
#include <ao/utility/ByteView.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ao::library::test
{
  namespace
  {
    constexpr std::size_t kCustomValueBytes = 64'000;
    constexpr std::size_t kShortUriBytes = 9;
    constexpr std::size_t kUriComponentBytes = 249;
    constexpr std::size_t kUriComponentCount = 6;
    constexpr std::size_t kDestinationUriBytes = (kUriComponentCount * kUriComponentBytes) + (kUriComponentCount - 1);
    constexpr std::size_t kCustomPayloadBytes =
      sizeof(CustomMetadataBlockHeader) + sizeof(CustomMetadataEntry) + kCustomValueBytes;
    constexpr std::size_t kOldUnalignedBytes =
      sizeof(TrackColdHeader) + kCustomPayloadBytes + sizeof(TrackSegmentBlock) + kShortUriBytes;
    constexpr std::size_t kOldRecordBytes = (kOldUnalignedBytes + 3) & ~std::size_t{3};
    constexpr std::size_t kNewUnalignedBytes =
      sizeof(TrackColdHeader) + kCustomPayloadBytes + sizeof(TrackSegmentBlock) + kDestinationUriBytes;
    constexpr std::size_t kNewRecordBytes = (kNewUnalignedBytes + 3) & ~std::size_t{3};
    constexpr auto kShortUri = std::string_view{"disc.flac"};
    constexpr auto kWholeFileUri = std::string_view{"whole.flac"};

    // The relink counterexample is a format boundary, pinned independently of the
    // production limit constant: a 9-byte URI fits, and the 1,499-byte canonical
    // destination pads to 65,568 bytes.
    static_assert(kShortUri.size() == kShortUriBytes);
    static_assert(kDestinationUriBytes == 1'499);
    static_assert(kDestinationUriBytes < 1'976);
    static_assert(kCustomPayloadBytes == 64'016);
    static_assert(kCustomPayloadBytes % 4 == 0);
    static_assert(sizeof(TrackSegmentBlock) % 4 == 0);
    static_assert(kOldUnalignedBytes == 64'077);
    static_assert(kOldRecordBytes == 64'080);
    static_assert(kOldRecordBytes <= 65'535);
    static_assert(kNewUnalignedBytes == 65'567);
    static_assert(kNewRecordBytes == 65'568);
    static_assert(kNewRecordBytes > 65'535);
    static_assert(offsetof(TrackColdHeader, duration) == 0);
    static_assert(sizeof(TrackDuration) == 4);

    TrackBuilder carrierMember(std::string_view artist,
                               std::string_view uri,
                               std::uint16_t ordinal,
                               std::uint64_t startFrame,
                               std::uint64_t frameCount)
    {
      auto builder = TrackBuilder::makeEmpty();
      builder.metadata().title("Segment").artist(artist);
      builder.property()
        .uri(uri)
        .sampleRate(SampleRate{44100})
        .segment(TrackSegment{.startFrame = startFrame, .frameCount = frameCount, .ordinal = ordinal});
      return builder;
    }

    std::size_t mismatchedByteCount(std::span<std::byte const> lhs, std::span<std::byte const> rhs)
    {
      std::size_t count = 0;
      auto const shared = std::min(lhs.size(), rhs.size());

      for (std::size_t index = 0; index < shared; ++index)
      {
        if (lhs[index] != rhs[index])
        {
          ++count;
        }
      }

      return count + (lhs.size() - shared) + (rhs.size() - shared);
    }

    std::string oversizedDestinationUri()
    {
      constexpr auto kExtension = std::string_view{".flac"};
      auto uri = std::string{};
      uri.reserve(kDestinationUriBytes);

      for (std::size_t index = 0; index < kUriComponentCount; ++index)
      {
        if (index != 0)
        {
          uri.push_back('/');
        }

        if (index + 1 == kUriComponentCount)
        {
          uri.append(kUriComponentBytes - kExtension.size(), 'b');
          uri.append(kExtension);
          continue;
        }

        uri.append(kUriComponentBytes, 'a');
      }

      return uri;
    }

    template<typename Function>
    void replaceColdTrack(std::filesystem::path const& path, TrackId id, Function function)
    {
      auto environment = lmdb::test::openEnvironment(path, {.flags = lmdb::kEnvNoTls, .maxDatabases = 8});
      auto transaction = lmdb::test::beginWriteTransaction(environment);
      auto database = lmdb::test::openIntegerKeyDatabase(transaction, "tracks_cold");
      auto writer = database.writer(transaction);
      auto const optBytes = writer.get(id.raw());
      REQUIRE(optBytes);
      auto bytes = std::vector<std::byte>{optBytes->begin(), optBytes->end()};
      function(bytes);
      REQUIRE(writer.update(id.raw(), bytes));
      REQUIRE(transaction.commit());
    }
  } // namespace

  TEST_CASE("TrackWriter - createCarrier rejects a noncanonical first member URI before staging",
            "[library][unit][track-writer]")
  {
    // ./disc.flac parses, but manifest lookup aborts unless the caller's spelling
    // is already canonical. Rejection must precede that lookup and any staging.
    auto const parsedRes = LibraryUri::parse("./disc.flac");
    REQUIRE(parsedRes);
    CHECK(parsedRes->value() == "disc.flac");

    auto const temp = ao::test::TempDir{};
    auto library = makeTestMusicLibrary(temp.path(), temp.path() / "db");
    std::uint64_t committedRevision = 0;

    {
      auto read = library.readTransaction();
      committedRevision = library.libraryRevision(read);
      CHECK(library.tracks().reader(read).entryCount() == 0);
      CHECK(library.dictionary().size() == 0);
    }

    auto const rejected = std::array{carrierMember("Must not be interned", "./disc.flac", 2, 588, 588),
                                     carrierMember("Must not be interned", "./disc.flac", 1, 0, 588)};

    {
      auto transaction = writeTransaction(library);
      auto const rejectedRes = transaction.apply(
        [&library, &rejected](LibraryWrite& write) -> Result<>
        {
          auto const createRes = write.tracks().createCarrier(rejected, FileManifestBuilder::makeEmpty(), 1176);
          REQUIRE_FALSE(createRes);
          CHECK(createRes.error().code == Error::Code::InvalidInput);
          CHECK(createRes.error().message == "Carrier URI is not canonical");
          CHECK(library.tracks().reader(write).entryCount() == 0);
          CHECK_FALSE(write.tracks().manifest("disc.flac"));
          CHECK(library.manifest().reader(write).begin() == FileManifestStore::Reader::EndSentinel{});
          CHECK(library.dictionary().size() == 0);
          CHECK_FALSE(library.dictionary().findId("Must not be interned"));
          return std::unexpected{createRes.error()};
        });
      REQUIRE_FALSE(rejectedRes);
      CHECK(rejectedRes.error().code == Error::Code::InvalidInput);
      CHECK(rejectedRes.error().message == "Carrier URI is not canonical");
    }

    {
      auto read = library.readTransaction();
      CHECK(library.libraryRevision(read) == committedRevision);
      CHECK(library.tracks().reader(read).entryCount() == 0);
      CHECK(library.manifest().reader(read).begin() == FileManifestStore::Reader::EndSentinel{});
      CHECK_FALSE(library.dictionary().findId("Must not be interned"));
    }

    auto const accepted = std::array{carrierMember("Admitted artist", "disc.flac", 2, 588, 588),
                                     carrierMember("Admitted artist", "disc.flac", 1, 0, 588)};
    auto transaction = writeTransaction(library);
    auto const idsRes =
      transaction.apply([&accepted](LibraryWrite& write)
                        { return write.tracks().createCarrier(accepted, FileManifestBuilder::makeEmpty(), 1176); });
    REQUIRE(idsRes);
    CHECK(*idsRes == std::vector<TrackId>{TrackId{1}, TrackId{2}});
    REQUIRE(transaction.commit());

    auto read = library.readTransaction();
    CHECK(library.tracks().reader(read).entryCount() == 2);
    auto const optManifest = library.manifest().reader(read).get("disc.flac");
    REQUIRE(optManifest);
    CHECK(std::ranges::equal(optManifest->segmentTrackIds(), std::array{TrackId{2}, TrackId{1}}));
    CHECK(library.manifest().reader(read).begin() != FileManifestStore::Reader::EndSentinel{});
    CHECK(library.dictionary().findId("Admitted artist") == DictionaryId{1});
    CHECK_FALSE(library.dictionary().findId("Must not be interned"));
    auto const optFirst = library.tracks().reader(read).get(TrackId{1});
    REQUIRE(optFirst);
    CHECK(optFirst->property().uri() == "disc.flac");
    CHECK(optFirst->property().segment() == TrackSegment{.startFrame = 588, .frameCount = 588, .ordinal = 2});
  }

  TEST_CASE("validateSerializedColdTrack rejects a negative whole-file duration", "[library][unit][track-integrity]")
  {
    auto const positive = makeColdData(TrackColdHeader{.duration = TrackDuration{1000}}, kWholeFileUri);
    auto const negative = makeColdData(TrackColdHeader{.duration = TrackDuration{-1}}, kWholeFileUri);
    REQUIRE(positive.size() == negative.size());
    REQUIRE(utility::bytes::isAligned(positive.data(), 4));
    REQUIRE(utility::bytes::isAligned(negative.data(), 4));
    CHECK(mismatchedByteCount(positive, negative) == sizeof(TrackDuration));
    CHECK(std::ranges::equal(std::span<std::byte const>{positive}.subspan(sizeof(TrackDuration)),
                             std::span<std::byte const>{negative}.subspan(sizeof(TrackDuration))));
    CHECK(validateSerializedColdTrack(positive));

    auto const negativeRes = validateSerializedColdTrack(negative);
    REQUIRE_FALSE(negativeRes);
    CHECK(negativeRes.error().code == Error::Code::CorruptData);
    CHECK(negativeRes.error().message == "Cold Track record contains a negative duration");

    // The shallow gate and deep structural reader still expose the stored value.
    // Open admission, not view safety, owns this rejection.
    auto const view = TrackView{std::span<std::byte const>{}, negative};
    CHECK(view.isColdValid());
    CHECK(view.property().duration() == std::chrono::milliseconds{-1});
    CHECK(detail::TrackColdReader{negative}.isValid());

    CHECK(validateSerializedColdTrack(makeColdData(TrackColdHeader{.duration = TrackDuration{0}}, kWholeFileUri)));
    CHECK(validateSerializedColdTrack(makeColdData(TrackColdHeader{.duration = TrackDuration::max()}, kWholeFileUri)));

    auto const minimumRes =
      validateSerializedColdTrack(makeColdData(TrackColdHeader{.duration = TrackDuration::min()}, kWholeFileUri));
    REQUIRE_FALSE(minimumRes);
    CHECK(minimumRes.error().code == Error::Code::CorruptData);
    CHECK(minimumRes.error().message == "Cold Track record contains a negative duration");
  }

  TEST_CASE("MusicLibrary - open rejects a whole file whose duration bytes were changed to negative",
            "[library][unit][track-integrity]")
  {
    auto const temp = ao::test::TempDir{};
    auto const databasePath = temp.path() / "db";
    auto trackId = kInvalidTrackId;

    {
      auto library = makeTestMusicLibrary(temp.path(), databasePath);
      trackId = addTrack(
        library,
        TrackSpec{.title = "Whole", .uri = std::string{kWholeFileUri}, .duration = std::chrono::milliseconds{1000}});
    }

    REQUIRE(trackId == TrackId{1});

    {
      auto library = makeTestMusicLibrary(temp.path(), databasePath);
      auto read = library.readTransaction();
      auto const optView = library.tracks().reader(read).get(trackId);
      REQUIRE(optView);
      CHECK_FALSE(optView->property().segment());
      CHECK(optView->property().duration() == std::chrono::milliseconds{1000});
    }

    replaceColdTrack(databasePath,
                     trackId,
                     [](std::vector<std::byte>& bytes)
                     {
                       REQUIRE(utility::bytes::isAligned(bytes.data(), alignof(TrackColdHeader)));
                       auto const original = bytes;
                       auto* header = utility::layout::viewMutable<TrackColdHeader>(bytes);
                       REQUIRE(header->blockOffsets[trackColdBlockSlotIndex(TrackColdBlockSlot::Segment)] == 0);
                       REQUIRE(header->duration == TrackDuration{1000});
                       header->duration = TrackDuration{-1};
                       CHECK(mismatchedByteCount(original, bytes) == sizeof(TrackDuration));
                       CHECK(std::ranges::equal(std::span<std::byte const>{original}.subspan(sizeof(TrackDuration)),
                                                std::span<std::byte const>{bytes}.subspan(sizeof(TrackDuration))));
                     });

    auto const openRes = openTestMusicLibrary(temp.path(), databasePath);
    REQUIRE_FALSE(openRes);
    CHECK(openRes.error().code == Error::Code::CorruptData);
    CHECK(openRes.error().message ==
          "Track 1 failed persisted validation: Cold Track record contains a negative duration");
  }

  TEST_CASE("WriteTransaction - oversized carrier relink returns ValueTooLarge and rolls back staged work",
            "[library][unit][write-transaction]")
  {
    auto const destinationUri = oversizedDestinationUri();
    auto const parsedRes = LibraryUri::parse(destinationUri);
    REQUIRE(parsedRes);
    CHECK(parsedRes->value() == destinationUri);
    CHECK(destinationUri.size() == kDestinationUriBytes);

    auto const temp = ao::test::TempDir{};
    auto library = makeTestMusicLibrary(temp.path(), temp.path() / "db");
    auto const customValue = std::string(kCustomValueBytes, 'v');
    auto builder = TrackBuilder::makeEmpty();
    builder.metadata().title("Carrier");
    builder.customMetadata().add("key", customValue);
    builder.property()
      .uri(kShortUri)
      .sampleRate(SampleRate{44100})
      .segment(TrackSegment{.startFrame = 0, .frameCount = 588, .ordinal = 1});
    auto const tracks = std::array{builder};
    auto memberId = kInvalidTrackId;

    {
      auto transaction = writeTransaction(library);
      auto const idsRes = transaction.apply(
        [&tracks](LibraryWrite& write)
        { return write.tracks().createCarrier(tracks, FileManifestBuilder::makeEmpty().fileSize(12345), 588); });
      REQUIRE(idsRes);
      REQUIRE(idsRes->size() == 1);
      memberId = idsRes->front();
      REQUIRE(transaction.commit());
    }

    REQUIRE(memberId == TrackId{1});
    std::uint64_t committedRevision = 0;

    {
      auto read = library.readTransaction();
      committedRevision = library.libraryRevision(read);
      auto const optView = library.tracks().reader(read).get(memberId);
      REQUIRE(optView);
      CHECK(optView->metadata().title() == "Carrier");
      CHECK(optView->property().uri() == kShortUri);
      auto const optKeyId = library.dictionary().findId("key");
      REQUIRE(optKeyId);
      auto const optValue = optView->customMetadata().get(*optKeyId);
      REQUIRE(optValue);
      CHECK(*optValue == customValue);
      auto const optManifest = library.manifest().reader(read).get(kShortUri);
      REQUIRE(optManifest);
      CHECK(optManifest->fileSize() == 12345);
      CHECK(std::ranges::equal(optManifest->segmentTrackIds(), std::array{memberId}));
    }

    auto writable = ao::test::requireValue(WritableMusicLibrary::acquire(library));
    auto transaction = writable.writeTransaction();
    auto stagedListId = kInvalidListId;
    auto const relinkRes = transaction.apply(
      [&library, &destinationUri, memberId, &stagedListId](LibraryWrite& write) -> Result<>
      {
        auto const listRes = write.lists().create(ListBuilder::makeEmpty().name("Staged"));
        REQUIRE(listRes);
        stagedListId = *listRes;
        REQUIRE(library.lists().reader(write).get(stagedListId));

        auto writer = write.tracks();
        auto const optView = writer.get(memberId);
        REQUIRE(optView);
        auto hot = TrackBuilder::fromHotView(*optView, library.dictionary());
        hot.metadata().title("Must roll back");
        REQUIRE(writer.updateHot(memberId, hot));
        return writer.relinkCarrier(kShortUri, destinationUri, FileManifestBuilder::makeEmpty());
      });
    REQUIRE_FALSE(relinkRes);
    CHECK(relinkRes.error().code == Error::Code::ValueTooLarge);
    CHECK(relinkRes.error().message == "URI would make the padded cold Track record exceed 65535 bytes");

    auto retry = writable.writeTransaction();
    retry.abort();

    auto read = library.readTransaction();
    CHECK(library.libraryRevision(read) == committedRevision);
    CHECK_FALSE(library.lists().reader(read).get(stagedListId));
    CHECK(library.lists().reader(read).begin() == ListStore::Reader::EndSentinel{});
    auto const optView = library.tracks().reader(read).get(memberId);
    REQUIRE(optView);
    CHECK(optView->metadata().title() == "Carrier");
    CHECK(optView->property().uri() == kShortUri);
    auto const optKeyId = library.dictionary().findId("key");
    REQUIRE(optKeyId);
    auto const optValue = optView->customMetadata().get(*optKeyId);
    REQUIRE(optValue);
    CHECK(*optValue == customValue);
    auto const optManifest = library.manifest().reader(read).get(kShortUri);
    REQUIRE(optManifest);
    CHECK(optManifest->fileSize() == 12345);
    CHECK(std::ranges::equal(optManifest->segmentTrackIds(), std::array{memberId}));
    CHECK_FALSE(library.manifest().reader(read).get(destinationUri));
  }
} // namespace ao::library::test
