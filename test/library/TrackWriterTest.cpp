// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/library/TrackWriter.h>

#include "test/TestFixtureSupport.h"
#include "test/library/MusicLibraryTestSupport.h"
#include "test/library/TrackTestSupport.h"
#include "test/library/WritableLibraryTestSupport.h"
#include <ao/AudioScalars.h>
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/FileTimestamp.h>
#include <ao/PictureType.h>
#include <ao/library/Credits.h>
#include <ao/library/DictionaryStore.h>
#include <ao/library/FileManifestBuilder.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/FileStatus.h>
#include <ao/library/LibraryUri.h>
#include <ao/library/LibraryWrite.h>
#include <ao/library/ListStore.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/ResourceStore.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackLayout.h>
#include <ao/library/TrackStore.h>
#include <ao/library/TrackView.h>
#include <ao/library/WriteTransaction.h>
#include <ao/library/detail/TrackViewRawAccess.h>
#include <ao/utility/ByteView.h>
#include <ao/utility/Sha256.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace ao::library::test
{
  namespace
  {
    template<typename Store>
    concept HasPublicPhysicalWriter =
      requires(Store const& store, WriteTransaction& transaction) { store.writer(transaction); };

    template<typename Transaction>
    concept HasPublicDictionaryWriter = requires(Transaction& transaction) { transaction.dictionary(); };

    template<typename Builder>
    concept HasPublicTrackPreparation =
      requires(Builder const& builder, WriteTransaction& transaction, ResourceStore const& resources) {
        builder.prepare(transaction, resources);
        builder.prepareHot(transaction);
        builder.prepareCold(transaction, resources);
      };

    template<typename Transaction>
    concept HasPublicLogicalMutationPort = requires(Transaction& transaction) {
      transaction.tracks();
      transaction.lists();
    };

    template<typename Writer>
    concept AcceptsPreparedManifest =
      requires(Writer& writer, FileManifestBuilder::Prepared const& prepared) { writer.updateManifest(prepared); };

    template<typename Write>
    concept CanCommit = requires(Write& write) { write.commit(); };

    template<typename Write>
    concept CanAbort = requires(Write& write) { write.abort(); };

    static_assert(!HasPublicPhysicalWriter<TrackStore>);
    static_assert(!HasPublicPhysicalWriter<ListStore>);
    static_assert(!HasPublicPhysicalWriter<FileManifestStore>);
    static_assert(!HasPublicPhysicalWriter<ResourceStore>);
    static_assert(!HasPublicDictionaryWriter<WriteTransaction>);
    static_assert(!HasPublicTrackPreparation<TrackBuilder>);
    static_assert(!HasPublicLogicalMutationPort<WriteTransaction>);
    static_assert(!AcceptsPreparedManifest<TrackWriter>);
    static_assert(!std::is_copy_constructible_v<TrackWriter>);
    static_assert(!std::is_default_constructible_v<LibraryWrite>);
    static_assert(!std::is_copy_constructible_v<LibraryWrite>);
    static_assert(!std::is_move_constructible_v<LibraryWrite>);
    static_assert(!CanCommit<LibraryWrite>);
    static_assert(!CanAbort<LibraryWrite>);

    TrackBuilder loadForEdit(TrackWriter const& writer,
                             TrackId id,
                             TrackWriter::EditTier tier = TrackWriter::EditTier::Complete)
    {
      auto optBuilder = writer.loadForEdit(id, tier);
      REQUIRE(optBuilder);
      return *std::move(optBuilder);
    }

    void checkPreviousNumbers(TrackNumberPatch const& actual, TrackNumberPatch const& expected)
    {
      CHECK(actual.optYear == expected.optYear);
      CHECK(actual.optTrackNumber == expected.optTrackNumber);
      CHECK(actual.optTrackTotal == expected.optTrackTotal);
      CHECK(actual.optDiscNumber == expected.optDiscNumber);
      CHECK(actual.optDiscTotal == expected.optDiscTotal);
    }

    DictionaryId soleRoleId(TrackView const& view)
    {
      auto const entries = view.performance().credits();
      REQUIRE(entries.size() == 1);
      return entries.front().roleId;
    }

    CreditView soleCredit(TrackBuilder const& builder)
    {
      auto const entries = builder.metadata().credits();
      REQUIRE(entries.size() == 1);
      return entries.front();
    }

    void setRole(TrackWriter& writer, TrackId id, std::string_view role)
    {
      auto builder = loadForEdit(writer, id);
      auto credit = soleCredit(builder);
      credit.role = role;
      builder.metadata().credits(std::array{credit});
      REQUIRE(writer.updateCold(id, builder));
    }
  } // namespace

  TEST_CASE("TrackWriter - create commits one Track, manifest, dictionary, and Resource graph")
  {
    auto const temp = ao::test::TempDir{};
    auto library = makeTestMusicLibrary(temp.path(), temp.path() / "db");
    auto transaction = writeTransaction(library);
    auto coverBytes = std::array{std::byte{1}, std::byte{2}, std::byte{3}};
    auto track = TrackBuilder::makeEmpty();
    track.metadata().title("Logical Track").artist("Logical Artist");
    track.property().uri("logical.flac");
    track.tags().add("favorite");
    track.coverArt().add(PictureType::FrontCover, std::span<std::byte const>{coverBytes});
    constexpr auto kMtime = FileTimestamp{.seconds = 1719835200, .nanoseconds = 654321000};
    auto manifest = FileManifestBuilder::makeEmpty();
    manifest.fileSize(321).mtime(kMtime).status(FileStatus::Available);

    auto createRes =
      transaction.apply([&track, &manifest](LibraryWrite& write) { return write.tracks().create(track, manifest); });

    REQUIRE(createRes);
    auto const trackId = *createRes;
    REQUIRE(transaction.commit());

    auto readTransaction = library.readTransaction();
    auto const optTrack = library.tracks().reader(readTransaction).get(trackId, TrackStore::Reader::LoadMode::Both);
    REQUIRE(optTrack);
    CHECK(optTrack->metadata().title() == "Logical Track");
    CHECK(library.dictionary().getOrDefault(optTrack->metadata().artistId()) == "Logical Artist");
    REQUIRE(optTrack->coverArt().primary());
    auto const resourceId = optTrack->coverArt().primary()->resourceId;
    auto const optResource = library.resources().reader(readTransaction).get(resourceId);
    REQUIRE(optResource);
    CHECK(optResource->digest == utility::computeSha256(coverBytes));
    CHECK(optResource->byteLength == coverBytes.size());

    auto const optManifest = library.manifest().reader(readTransaction).get("logical.flac");
    REQUIRE(optManifest);
    CHECK(optManifest->trackId() == trackId);
    CHECK(optManifest->fileSize() == 321);
    CHECK(optManifest->mtime() == kMtime);
  }

  TEST_CASE("TrackWriter - a maximum-length URI survives commit and reopen")
  {
    auto const temp = ao::test::TempDir{};
    auto uri = std::string(200, 'a');

    for (std::size_t index = 1; index < 9; ++index)
    {
      uri += "/" + std::string(200, 'a');
    }

    uri += "/" + std::string(LibraryUri::kMaxLength - uri.size() - 6, 'b') + ".flac";
    REQUIRE(uri.size() == LibraryUri::kMaxLength);

    auto trackId = kInvalidTrackId;

    {
      auto library = makeTestMusicLibrary(temp.path(), temp.path() / "db");
      trackId = addTrack(library, TrackSpec{.title = "Longest", .uri = uri});
    }

    // Reopening admits the cold record's URI and its manifest binding.
    auto library = makeTestMusicLibrary(temp.path(), temp.path() / "db");
    auto readTransaction = library.readTransaction();
    auto const optTrack = library.tracks().reader(readTransaction).get(trackId, TrackStore::Reader::LoadMode::Both);
    REQUIRE(optTrack);
    CHECK(optTrack->property().uri() == uri);
    auto const optManifest = library.manifest().reader(readTransaction).get(uri);
    REQUIRE(optManifest);
    CHECK(optManifest->trackId() == trackId);
  }

  TEST_CASE("TrackWriter - create rejects a missing existing Resource before mutation")
  {
    auto const temp = ao::test::TempDir{};
    auto library = makeTestMusicLibrary(temp.path(), temp.path() / "db");
    auto transaction = writeTransaction(library);
    auto track = TrackBuilder::makeEmpty();
    track.metadata().artist("Must not be interned");
    track.property().uri("missing-resource.flac");
    track.coverArt().add(PictureType::FrontCover, ResourceId{123456});

    auto createRes = transaction.apply(
      [&track](LibraryWrite& write) -> Result<TrackId>
      {
        auto writer = write.tracks();
        auto rejectedRes = writer.create(track, FileManifestBuilder::makeEmpty());
        REQUIRE_FALSE(rejectedRes);
        CHECK(rejectedRes.error().code == Error::Code::NotFound);

        auto fallback = TrackBuilder::makeEmpty();
        fallback.property().uri("accepted.flac");
        return writer.create(fallback, FileManifestBuilder::makeEmpty());
      });

    REQUIRE(createRes);
    REQUIRE(transaction.commit());

    auto readTransaction = library.readTransaction();
    CHECK(library.tracks().reader(readTransaction).entryCount() == 1);
    CHECK_FALSE(library.manifest().reader(readTransaction).get("missing-resource.flac"));
    REQUIRE(library.manifest().reader(readTransaction).get("accepted.flac"));
    CHECK(library.dictionary().size() == 0);
  }

  TEST_CASE("TrackWriter - create and update reject an oversized cold record before mutation")
  {
    auto const temp = ao::test::TempDir{};
    auto library = makeTestMusicLibrary(temp.path(), temp.path() / "db");
    auto const existingId = addTrack(library, TrackSpec{.title = "Existing", .uri = "existing.flac"});
    auto const dictionarySize = library.dictionary().size();
    auto const oversizedValue = std::string(kTrackColdRecordSizeLimit, 'v');
    auto transaction = writeTransaction(library);

    auto createRes = transaction.apply(
      [&oversizedValue, existingId](LibraryWrite& write) -> Result<TrackId>
      {
        auto writer = write.tracks();
        auto created = TrackBuilder::makeEmpty();
        created.metadata().artist("Must not be interned");
        created.property().uri("oversized.flac");
        created.customMetadata().add("oversized-key", oversizedValue);
        auto const createRejectedRes = writer.create(created, FileManifestBuilder::makeEmpty());
        REQUIRE_FALSE(createRejectedRes);
        CHECK(createRejectedRes.error().code == Error::Code::ValueTooLarge);

        auto updated = loadForEdit(writer, existingId);
        updated.metadata().artist("Must not be interned");
        updated.customMetadata().add("oversized-key", oversizedValue);
        auto const updateRejectedRes = writer.update(existingId, updated);
        REQUIRE_FALSE(updateRejectedRes);
        CHECK(updateRejectedRes.error().code == Error::Code::ValueTooLarge);

        auto fallback = TrackBuilder::makeEmpty();
        fallback.property().uri("accepted.flac");
        return writer.create(fallback, FileManifestBuilder::makeEmpty());
      });

    REQUIRE(createRes);
    REQUIRE(transaction.commit());

    auto readTransaction = library.readTransaction();
    CHECK(library.tracks().reader(readTransaction).entryCount() == 2);
    CHECK_FALSE(library.manifest().reader(readTransaction).get("oversized.flac"));
    auto const optExisting = library.tracks().reader(readTransaction).get(existingId);
    REQUIRE(optExisting);
    CHECK(library.dictionary().getOrDefault(optExisting->metadata().artistId()) == "Artist");
    CHECK(optExisting->customMetadata().empty());
    CHECK(library.dictionary().size() == dictionarySize);
  }

  TEST_CASE("TrackWriter - create rejects a duplicate manifest URI without aliasing it")
  {
    auto const temp = ao::test::TempDir{};
    auto library = makeTestMusicLibrary(temp.path(), temp.path() / "db");
    auto const firstId = addTrack(library, TrackSpec{.title = "First", .uri = "shared.flac"});
    auto transaction = writeTransaction(library);
    auto duplicate = TrackBuilder::makeEmpty();
    duplicate.metadata().title("Duplicate");
    duplicate.property().uri("shared.flac");

    auto createRes = transaction.apply([&duplicate](LibraryWrite& write)
                                       { return write.tracks().create(duplicate, FileManifestBuilder::makeEmpty()); });

    REQUIRE_FALSE(createRes);
    CHECK(createRes.error().code == Error::Code::Conflict);
    auto readTransaction = library.readTransaction();
    CHECK(library.tracks().reader(readTransaction).entryCount() == 1);
    auto const optTrack = library.tracks().reader(readTransaction).get(firstId);
    REQUIRE(optTrack);
    CHECK(optTrack->metadata().title() == "First");
    auto const optManifest = library.manifest().reader(readTransaction).get("shared.flac");
    REQUIRE(optManifest);
    CHECK(optManifest->trackId() == firstId);
  }

  TEST_CASE("TrackWriter - validate accepts complete builders and rejects hot-only builders as input")
  {
    auto const temp = ao::test::TempDir{};
    auto library = makeTestMusicLibrary(temp.path(), temp.path() / "db");
    auto const trackId = addTrack(library, TrackSpec{.title = "Stored", .uri = "stored.flac"});
    auto transaction = writeTransaction(library);

    REQUIRE(transaction.apply(
      [&](LibraryWrite& write) -> Result<>
      {
        auto writer = write.tracks();
        auto complete = loadForEdit(writer, trackId);
        REQUIRE(writer.validate(complete));

        auto hotOnly = loadForEdit(writer, trackId, TrackWriter::EditTier::Hot);
        auto hotOnlyRes = writer.validate(hotOnly);
        REQUIRE_FALSE(hotOnlyRes);
        CHECK(hotOnlyRes.error().code == Error::Code::InvalidInput);
        return {};
      }));
    REQUIRE(transaction.commit());
  }

  TEST_CASE("TrackWriter - full, cold, and replacement updates persist their owned sides")
  {
    auto const temp = ao::test::TempDir{};
    auto library = makeTestMusicLibrary(temp.path(), temp.path() / "db");
    auto const trackId = addTrack(library, TrackSpec{.title = "Before", .uri = "updates.flac"});
    auto transaction = writeTransaction(library);

    auto expectedTitle = std::string_view{"Before"};
    auto expectedDuration = std::chrono::seconds{200};
    std::uint64_t expectedFileSize = 0;
    auto expectedStatus = FileStatus::Available;

    SECTION("full update persists both sides")
    {
      expectedTitle = "Full update";
      expectedDuration = std::chrono::seconds{210};
      REQUIRE(transaction.apply(
        [&](LibraryWrite& write)
        {
          auto writer = write.tracks();
          auto full = loadForEdit(writer, trackId);
          full.metadata().title("Full update");
          full.property().duration(std::chrono::seconds{210});
          return writer.update(trackId, full);
        }));
    }

    SECTION("cold update does not replace hot metadata")
    {
      expectedDuration = std::chrono::seconds{220};
      REQUIRE(transaction.apply(
        [&](LibraryWrite& write)
        {
          auto writer = write.tracks();
          auto cold = loadForEdit(writer, trackId);
          cold.metadata().title("Ignored by cold update");
          cold.property().duration(std::chrono::seconds{220});
          return writer.updateCold(trackId, cold);
        }));
    }

    SECTION("replacement persists both sides and manifest facts")
    {
      expectedTitle = "Replacement";
      expectedDuration = std::chrono::seconds{230};
      expectedFileSize = 987;
      expectedStatus = FileStatus::Missing;
      REQUIRE(transaction.apply(
        [&](LibraryWrite& write)
        {
          auto writer = write.tracks();
          auto replacement = loadForEdit(writer, trackId);
          replacement.metadata().title("Replacement");
          replacement.property().duration(std::chrono::seconds{230});
          auto const optManifest = writer.manifest("updates.flac");
          REQUIRE(optManifest);
          auto manifest = FileManifestBuilder::fromView(*optManifest);
          manifest.fileSize(987).status(FileStatus::Missing);
          return writer.replace(trackId, replacement, manifest);
        }));
    }

    REQUIRE(transaction.commit());
    auto readTransaction = library.readTransaction();
    auto const optTrack = library.tracks().reader(readTransaction).get(trackId, TrackStore::Reader::LoadMode::Both);
    REQUIRE(optTrack);
    CHECK(optTrack->metadata().title() == expectedTitle);
    CHECK(optTrack->property().duration() == expectedDuration);
    auto const optManifest = library.manifest().reader(readTransaction).get("updates.flac");
    REQUIRE(optManifest);
    CHECK(optManifest->trackId() == trackId);
    CHECK(optManifest->fileSize() == expectedFileSize);
    CHECK(optManifest->status() == expectedStatus);
  }

  TEST_CASE("TrackWriter - an explicit title clear survives tag rebuild and cold and full updates")
  {
    auto const temp = ao::test::TempDir{};
    auto const databasePath = temp.path() / "db";
    auto trackId = kInvalidTrackId;

    {
      auto library = makeTestMusicLibrary(temp.path(), databasePath);
      trackId = addTrack(library, TrackSpec{.title = "Before", .uri = "cleared.flac", .tags = {"old"}});
      auto transaction = writeTransaction(library);
      REQUIRE(transaction.apply(
        [&](LibraryWrite& write) -> Result<>
        {
          auto writer = write.tracks();

          {
            auto builder = loadForEdit(writer, trackId);
            builder.metadata().clearTitleExplicitly();
            REQUIRE(writer.updateHot(trackId, builder));
          }

          auto hot = loadForEdit(writer, trackId, TrackWriter::EditTier::Hot);
          CHECK(hot.metadata().isTitleExplicitlyCleared());
          hot.tags().clear().add("first").add("second");
          return writer.updateHot(trackId, hot);
        }));
      REQUIRE(transaction.commit());

      transaction = writeTransaction(library);
      REQUIRE(transaction.apply(
        [&](LibraryWrite& write) -> Result<>
        {
          auto writer = write.tracks();

          {
            auto cold = loadForEdit(writer, trackId);
            cold.metadata().title("Ignored by cold update");
            cold.property().duration(std::chrono::seconds{225});
            REQUIRE(writer.updateCold(trackId, cold));
          }

          auto complete = loadForEdit(writer, trackId);
          CHECK(complete.metadata().isTitleExplicitlyCleared());
          complete.metadata().year(2001);
          return writer.update(trackId, complete);
        }));
      REQUIRE(transaction.commit());
    }

    auto library = makeTestMusicLibrary(temp.path(), databasePath);
    auto readTransaction = library.readTransaction();
    auto const optTrack = library.tracks().reader(readTransaction).get(trackId, TrackStore::Reader::LoadMode::Both);
    REQUIRE(optTrack);
    CHECK(optTrack->metadata().title().empty());
    CHECK(optTrack->metadata().isTitleExplicitlyCleared());
    CHECK(optTrack->metadata().year() == 2001);
    CHECK(optTrack->property().duration() == std::chrono::seconds{225});
    REQUIRE(optTrack->tags().count() == 2);
    CHECK(library.dictionary().get(optTrack->tags().id(0)) == "first");
    CHECK(library.dictionary().get(optTrack->tags().id(1)) == "second");
  }

  TEST_CASE("TrackWriter - mixed number edits preserve every other byte of a staged carrier member")
  {
    auto const temp = ao::test::TempDir{};
    auto const db = temp.path() / "db";
    auto id = kInvalidTrackId;
    auto roleId = kInvalidDictionaryId;
    auto expectedHot = std::vector<std::byte>{};
    auto expectedCold = std::vector<std::byte>{};
    auto const checkBytes = [&](TrackView const& view)
    {
      CHECK(std::ranges::equal(detail::TrackViewRawAccess::hotData(view), expectedHot));
      CHECK(std::ranges::equal(detail::TrackViewRawAccess::coldData(view), expectedCold));
    };

    {
      auto library = makeTestMusicLibrary(temp.path(), db);
      auto transaction = writeTransaction(library);
      auto const previousRes = transaction.apply(
        [&](LibraryWrite& write) -> Result<TrackNumberPatch>
        {
          auto members = std::array{TrackBuilder::makeEmpty()};
          auto& track = members.front();
          auto const credits = std::array{CreditView{.name = "Leader", .kind = CreditKind::Conductor},
                                          CreditView{.name = "Guest", .role = "Piano"},
                                          CreditView{.name = "Guest", .role = "Organ"}};
          track.metadata()
            .clearTitleExplicitly()
            .artist("Ensemble")
            .work("Suite")
            .recordingDate({.year = 1987, .month = 5})
            .credits(credits)
            .year(1992)
            .trackNumber(4)
            .discNumber(2)
            .discTotal(3);
          track.tags().add("live");
          track.customMetadata().add("room", "west hall");
          auto const cover = std::array{std::byte{0x37}, std::byte{0x41}};
          track.coverArt().add(PictureType::BackCover, cover);
          track.property()
            .uri("concert.flac")
            .sampleRate(SampleRate{44100})
            .segment(TrackSegment{.startFrame = 588, .frameCount = 588, .ordinal = 2});
          auto writer = write.tracks();
          auto const idsRes = writer.createCarrier(members, FileManifestBuilder::makeEmpty(), 1176);
          REQUIRE(idsRes);
          REQUIRE(idsRes->size() == 1);
          id = idsRes->front();

          auto const optBefore = writer.get(id);
          REQUIRE(optBefore);
          auto const performers = optBefore->performance().credits(CreditKind::Performer);
          REQUIRE(performers.size() == 2);
          roleId = performers.back().roleId;
          REQUIRE(roleId != kInvalidDictionaryId);
          CHECK_FALSE(library.dictionary().findId("Organ"));
          // Snapshot before the first replacement; both mapped sides may move.
          expectedHot = detail::TrackViewRawAccess::hotData(*optBefore) | std::ranges::to<std::vector<std::byte>>();
          expectedCold = detail::TrackViewRawAccess::coldData(*optBefore) | std::ranges::to<std::vector<std::byte>>();
          utility::layout::viewMutable<TrackHotHeader>(expectedHot)->year = 2026;
          auto* const cold = utility::layout::viewMutable<TrackColdHeader>(expectedCold);
          cold->trackNumber = 0;
          cold->trackTotal = 18;
          cold->discNumber = 1;

          auto const res = write.tracks().updateNumbers(
            id, {.optYear = 2026, .optTrackNumber = 0, .optTrackTotal = 18, .optDiscNumber = 1, .optDiscTotal = 3});
          REQUIRE(res);
          auto const optAfter = writer.get(id);
          REQUIRE(optAfter);
          checkBytes(*optAfter);
          CHECK_FALSE(library.dictionary().findId("Organ"));
          return res;
        });
      REQUIRE(previousRes);
      checkPreviousNumbers(
        *previousRes, {.optYear = 1992, .optTrackNumber = 4, .optTrackTotal = 0, .optDiscNumber = 2});
      REQUIRE(transaction.commit());
    }

    auto library = makeTestMusicLibrary(temp.path(), db);
    auto read = library.readTransaction();
    auto const optReopened = library.tracks().reader(read).get(id);
    REQUIRE(optReopened);
    checkBytes(*optReopened);
    CHECK(library.dictionary().findId("Organ") == roleId);
    auto const optManifest = library.manifest().reader(read).get("concert.flac");
    REQUIRE(optManifest);
    CHECK(optManifest->totalFrames() == 1176);
    CHECK(std::ranges::equal(optManifest->segmentTrackIds(), std::array{id}));
  }

  TEST_CASE("TrackWriter - numeric edits encode the same Track as a complete builder update")
  {
    auto const temp = ao::test::TempDir{};
    auto library = makeTestMusicLibrary(temp.path(), temp.path() / "db");
    auto const id = addTrack(library, TrackSpec{.tags = {"live"}, .customMetadata = {{"room", "hall"}}});
    auto hot = std::vector<std::byte>{};
    auto cold = std::vector<std::byte>{};
    {
      auto transaction = writeTransaction(library);
      REQUIRE(transaction.apply(
        [&](LibraryWrite& write) -> Result<>
        {
          auto writer = write.tracks();
          auto builder = loadForEdit(writer, id);
          builder.metadata().year(1998).trackNumber(0).trackTotal(12).discNumber(2).discTotal(3);
          REQUIRE(writer.update(id, builder));
          auto const optView = writer.get(id);
          REQUIRE(optView);
          hot = detail::TrackViewRawAccess::hotData(*optView) | std::ranges::to<std::vector<std::byte>>();
          cold = detail::TrackViewRawAccess::coldData(*optView) | std::ranges::to<std::vector<std::byte>>();
          return {};
        }));
      // Run the numeric writer against the same original bytes and dictionary IDs.
      transaction.abort();
    }

    auto transaction = writeTransaction(library);
    REQUIRE(transaction.apply(
      [&](LibraryWrite& write) -> Result<>
      {
        auto writer = write.tracks();
        auto const res = writer.updateNumbers(
          id, {.optYear = 1998, .optTrackNumber = 0, .optTrackTotal = 12, .optDiscNumber = 2, .optDiscTotal = 3});
        REQUIRE(res);
        checkPreviousNumbers(
          *res, {.optYear = 2020, .optTrackNumber = 1, .optTrackTotal = 0, .optDiscNumber = 1, .optDiscTotal = 0});
        auto const optView = writer.get(id);
        REQUIRE(optView);
        CHECK(std::ranges::equal(detail::TrackViewRawAccess::hotData(*optView), hot));
        CHECK(std::ranges::equal(detail::TrackViewRawAccess::coldData(*optView), cold));
        return {};
      }));
  }

  TEST_CASE("TrackWriter - number edits distinguish missing targets from unchanged numbers")
  {
    auto const temp = ao::test::TempDir{};
    auto library = makeTestMusicLibrary(temp.path(), temp.path() / "db");
    auto const id = addTrack(library, TrackSpec{.discTotal = 2});
    auto transaction = writeTransaction(library);
    auto const previousRes = transaction.apply(
      [&](LibraryWrite& write) -> Result<TrackNumberPatch>
      {
        auto writer = write.tracks();
        auto const patch = TrackNumberPatch{.optDiscTotal = 4};
        auto const missingRes = writer.updateNumbers(TrackId{1234}, {});
        REQUIRE_FALSE(missingRes);
        CHECK(missingRes.error().code == Error::Code::NotFound);
        auto const zeroRes = writer.updateNumbers(kInvalidTrackId, patch);
        REQUIRE_FALSE(zeroRes);
        CHECK(zeroRes.error().code == Error::Code::NotFound);

        checkPreviousNumbers(ao::test::requireValue(writer.updateNumbers(id, {})), {});
        auto const res = writer.updateNumbers(id, patch);
        REQUIRE(res);
        checkPreviousNumbers(ao::test::requireValue(writer.updateNumbers(id, patch)), {});
        return res;
      });
    REQUIRE(previousRes);
    checkPreviousNumbers(*previousRes, {.optDiscTotal = 2});
    REQUIRE(transaction.commit());
    auto const read = library.readTransaction();
    auto const optView = library.tracks().reader(read).get(id);
    REQUIRE(optView);
    CHECK(optView->metadata().discTotal() == 4);
  }

  TEST_CASE("TrackWriter - an unpublished credit role survives a same-write duration edit and reopen")
  {
    auto const temp = ao::test::TempDir{};
    auto const db = temp.path() / "db";
    auto id = kInvalidTrackId;
    auto stagedId = kInvalidDictionaryId;
    constexpr auto kDuration = std::chrono::seconds{225};
    auto expectedRole = std::string_view{"Pianist"};

    SECTION("duration edit only")
    {
    }

    SECTION("replace the role after the duration edit")
    {
      expectedRole = "Conductor";
    }

    {
      auto library = makeTestMusicLibrary(temp.path(), db);
      id = addTrack(library, TrackSpec{.credits = {{.name = "Performer"}}});
      auto const& dictionary = library.dictionary();
      REQUIRE(dictionary.findId("Performer"));
      auto transaction = writeTransaction(library);
      auto const observationRes = transaction.apply(
        [&](LibraryWrite& write) -> Result<std::pair<std::string, std::string>>
        {
          auto writer = write.tracks();
          auto const optSeed = writer.get(id);
          REQUIRE(optSeed);
          CHECK(soleRoleId(*optSeed) == kInvalidDictionaryId);
          CHECK(soleCredit(loadForEdit(writer, id)).role.empty());
          setRole(writer, id, "Pianist");

          auto other = write.tracks();
          auto const optStaged = other.get(id);
          REQUIRE(optStaged);
          stagedId = soleRoleId(*optStaged);
          REQUIRE(stagedId != kInvalidDictionaryId);
          CHECK_FALSE(dictionary.findId("Pianist"));
          CHECK(dictionary.getOrDefault(stagedId, "unpublished") == "unpublished");
          // Only the role is unpublished. The committed-only projection must not turn it into absence.
          auto const rejectedRes = TrackBuilder::fromCommittedCompleteView(*optStaged, dictionary);
          REQUIRE_FALSE(rejectedRes);
          CHECK(rejectedRes.error().code == Error::Code::InvalidInput);

          auto hot = loadForEdit(other, id, TrackWriter::EditTier::Hot);
          hot.tags().add("studio");
          REQUIRE(other.updateHot(id, hot));
          auto const stagedHot = loadForEdit(writer, id, TrackWriter::EditTier::Hot);
          REQUIRE(stagedHot.tags().names().size() == 1);
          CHECK(stagedHot.tags().names().front() == "studio");
          CHECK(stagedHot.metadata().credits().empty());
          CHECK_FALSE(dictionary.findId("studio"));

          auto complete = loadForEdit(other, id);
          CHECK(soleCredit(complete).name == "Performer");
          CHECK(soleCredit(complete).role == "Pianist");
          complete.property().duration(kDuration);
          REQUIRE(other.updateCold(id, complete));
          auto const optAfter = writer.get(id);
          REQUIRE(optAfter);
          CHECK(optAfter->property().duration() == kDuration);
          CHECK(soleRoleId(*optAfter) == stagedId);
          auto before = std::string{soleCredit(loadForEdit(other, id)).role};

          if (expectedRole != "Pianist")
          {
            setRole(writer, id, expectedRole);
          }

          CHECK_FALSE(dictionary.findId(expectedRole));
          return std::pair{std::move(before), std::string{soleCredit(loadForEdit(other, id)).role}};
        });
      REQUIRE(observationRes);
      CHECK(observationRes->first == "Pianist");
      CHECK(observationRes->second == expectedRole);
      REQUIRE(transaction.commit());
    }

    auto library = makeTestMusicLibrary(temp.path(), db);
    auto const& dictionary = library.dictionary();
    {
      auto const read = library.readTransaction();
      auto const optView = library.tracks().reader(read).get(id);
      REQUIRE(optView);
      CHECK(optView->property().duration() == kDuration);
      CHECK(dictionary.findId("Pianist") == stagedId);
      auto const roleId = soleRoleId(*optView);
      REQUIRE(roleId != kInvalidDictionaryId);
      CHECK(dictionary.findId(expectedRole) == roleId);
      CHECK((roleId == stagedId) == (expectedRole == "Pianist"));
      // Commit-first control: the row can now be decoded with the published dictionary alone.
      auto const projected = ao::test::requireValue(TrackBuilder::fromCommittedCompleteView(*optView, dictionary));
      CHECK(soleCredit(projected).role == expectedRole);
    }

    auto const generation = dictionary.generation();
    auto unchanged = writeTransaction(library);
    REQUIRE(unchanged.apply(
      [&](LibraryWrite& write)
      {
        auto writer = write.tracks();
        return writer.updateCold(id, loadForEdit(writer, id));
      }));
    REQUIRE(unchanged.commit());
    CHECK(dictionary.generation() == generation);
  }

  TEST_CASE("TrackWriter - a failed commit does not bind a reused role ID to its old text")
  {
    auto const temp = ao::test::TempDir{};
    auto library = makeTestMusicLibrary(temp.path(), temp.path() / "db");
    auto const id = addTrack(library, TrackSpec{.credits = {{.name = "Performer"}}});
    auto failed = writeTransaction(
      library, {.optInjectedCommitFailure = Error{.code = Error::Code::IoError, .message = "failed role commit"}});
    auto const failedRoleRes = failed.apply(
      [&](LibraryWrite& write) -> Result<DictionaryId>
      {
        auto writer = write.tracks();
        setRole(writer, id, "Discarded");
        CHECK(soleCredit(loadForEdit(writer, id)).role == "Discarded");
        auto const optView = writer.get(id);
        REQUIRE(optView);
        return soleRoleId(*optView);
      });
    REQUIRE(failedRoleRes);
    REQUIRE(*failedRoleRes != kInvalidDictionaryId);
    auto const commitRes = failed.commit();
    REQUIRE_FALSE(commitRes);
    CHECK(commitRes.error().code == Error::Code::IoError);

    auto retry = writeTransaction(library);
    REQUIRE(retry.apply(
      [&](LibraryWrite& write) -> Result<>
      {
        auto writer = write.tracks();
        CHECK(soleCredit(loadForEdit(writer, id)).role.empty());
        setRole(writer, id, "Pianist");
        auto const optView = writer.get(id);
        REQUIRE(optView);
        REQUIRE(soleRoleId(*optView) == *failedRoleRes);
        CHECK(soleCredit(loadForEdit(writer, id)).role == "Pianist");
        return {};
      }));
    REQUIRE(retry.commit());
    CHECK_FALSE(library.dictionary().findId("Discarded"));
    CHECK(library.dictionary().findId("Pianist") == *failedRoleRes);
  }

  TEST_CASE("TrackWriter - ordinary updates preserve the URI and manifest binding")
  {
    auto const temp = ao::test::TempDir{};
    auto library = makeTestMusicLibrary(temp.path(), temp.path() / "db");
    auto const trackId = addTrack(library, TrackSpec{.title = "Before", .uri = "before.flac"});

    {
      auto transaction = writeTransaction(library);
      REQUIRE(transaction.apply(
        [&](LibraryWrite& write) -> Result<>
        {
          auto writer = write.tracks();
          auto track = loadForEdit(writer, trackId);
          track.metadata().title("After");
          track.property().uri("ignored-by-hot-update.flac");

          if (auto updateRes = writer.updateHot(trackId, track); !updateRes)
          {
            return updateRes;
          }

          auto optManifest = writer.manifest("before.flac");
          REQUIRE(optManifest);
          auto manifest = FileManifestBuilder::fromView(*optManifest);
          manifest.trackId(TrackId{999}).status(FileStatus::Missing).fileSize(777);
          return writer.updateManifest(trackId, manifest);
        }));
      REQUIRE(transaction.commit());
    }

    auto readTransaction = library.readTransaction();
    auto const optTrack = library.tracks().reader(readTransaction).get(trackId, TrackStore::Reader::LoadMode::Both);
    REQUIRE(optTrack);
    CHECK(optTrack->metadata().title() == "After");
    CHECK(optTrack->property().uri() == "before.flac");
    auto const optManifest = library.manifest().reader(readTransaction).get("before.flac");
    REQUIRE(optManifest);
    CHECK(optManifest->trackId() == trackId);
    CHECK(optManifest->status() == FileStatus::Missing);
    CHECK(optManifest->fileSize() == 777);
  }

  TEST_CASE("TrackWriter - normal update rejects a URI change without committing partial state")
  {
    auto const temp = ao::test::TempDir{};
    auto library = makeTestMusicLibrary(temp.path(), temp.path() / "db");
    auto const trackId = addTrack(library, TrackSpec{.title = "Before", .uri = "before.flac"});
    auto transaction = writeTransaction(library);

    auto updateRes = transaction.apply(
      [&](LibraryWrite& write)
      {
        auto writer = write.tracks();
        auto track = loadForEdit(writer, trackId);
        track.metadata().title("Rejected");
        track.property().uri("after.flac");
        return writer.update(trackId, track);
      });

    REQUIRE_FALSE(updateRes);
    CHECK(updateRes.error().code == Error::Code::InvalidInput);

    auto readTransaction = library.readTransaction();
    auto const optTrack = library.tracks().reader(readTransaction).get(trackId, TrackStore::Reader::LoadMode::Both);
    REQUIRE(optTrack);
    CHECK(optTrack->metadata().title() == "Before");
    CHECK(optTrack->property().uri() == "before.flac");
    REQUIRE(library.manifest().reader(readTransaction).get("before.flac"));
    CHECK_FALSE(library.manifest().reader(readTransaction).get("after.flac"));
  }

  TEST_CASE("TrackWriter - relink replaces the manifest key while preserving Track identity")
  {
    auto const temp = ao::test::TempDir{};
    auto library = makeTestMusicLibrary(temp.path(), temp.path() / "db");
    auto const trackId = addTrack(library, TrackSpec{.title = "Moved", .uri = "old.flac"});
    auto transaction = writeTransaction(library);
    REQUIRE(transaction.apply(
      [&](LibraryWrite& write) -> Result<>
      {
        auto writer = write.tracks();
        auto track = loadForEdit(writer, trackId);
        track.property().uri("new.flac");
        auto optManifest = writer.manifest("old.flac");
        REQUIRE(optManifest);
        auto manifest = FileManifestBuilder::fromView(*optManifest);
        manifest.fileSize(999);
        return writer.relink(trackId, track, manifest);
      }));
    REQUIRE(transaction.commit());

    auto readTransaction = library.readTransaction();
    auto const optTrack = library.tracks().reader(readTransaction).get(trackId, TrackStore::Reader::LoadMode::Both);
    REQUIRE(optTrack);
    CHECK(optTrack->property().uri() == "new.flac");
    CHECK_FALSE(library.manifest().reader(readTransaction).get("old.flac"));
    auto const optManifest = library.manifest().reader(readTransaction).get("new.flac");
    REQUIRE(optManifest);
    CHECK(optManifest->trackId() == trackId);
    CHECK(optManifest->fileSize() == 999);
  }

  TEST_CASE("TrackWriter - delete removes its Track and manifest while preserving others")
  {
    auto const temp = ao::test::TempDir{};
    auto library = makeTestMusicLibrary(temp.path(), temp.path() / "db");
    auto const firstId = addTrack(library, TrackSpec{.title = "First", .uri = "first.flac"});
    auto const secondId = addTrack(library, TrackSpec{.title = "Second", .uri = "second.flac"});

    {
      auto transaction = writeTransaction(library);
      auto removeRes = transaction.apply([firstId](LibraryWrite& write) { return write.tracks().remove(firstId); });
      REQUIRE(removeRes);
      REQUIRE(*removeRes);
      REQUIRE(transaction.commit());
    }

    auto readTransaction = library.readTransaction();
    CHECK_FALSE(library.tracks().reader(readTransaction).get(firstId));
    CHECK_FALSE(library.manifest().reader(readTransaction).get("first.flac"));
    REQUIRE(library.tracks().reader(readTransaction).get(secondId));
    REQUIRE(library.manifest().reader(readTransaction).get("second.flac"));
  }

  TEST_CASE("TrackWriter - clear removes all Tracks and manifests")
  {
    auto const temp = ao::test::TempDir{};
    auto library = makeTestMusicLibrary(temp.path(), temp.path() / "db");
    addTrack(library, TrackSpec{.title = "First", .uri = "first.flac"});
    addTrack(library, TrackSpec{.title = "Second", .uri = "second.flac"});

    auto transaction = writeTransaction(library);
    REQUIRE(transaction.apply([](LibraryWrite& write) { return write.tracks().clear(); }));
    REQUIRE(transaction.commit());

    auto readTransaction = library.readTransaction();
    CHECK(library.tracks().reader(readTransaction).entryCount() == 0);
    CHECK(library.manifest().reader(readTransaction).begin() == FileManifestStore::Reader::EndSentinel{});
  }

  TEST_CASE("TrackWriter - deleting an absent Track succeeds with false")
  {
    auto const temp = ao::test::TempDir{};
    auto library = makeTestMusicLibrary(temp.path(), temp.path() / "db");
    auto transaction = writeTransaction(library);

    auto removeRes = transaction.apply([](LibraryWrite& write) { return write.tracks().remove(TrackId{4242}); });

    REQUIRE(removeRes);
    CHECK_FALSE(*removeRes);
    REQUIRE(transaction.commit());
  }
} // namespace ao::library::test
