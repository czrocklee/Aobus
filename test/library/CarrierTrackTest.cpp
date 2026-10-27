// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "lib/library/FileManifestValidation.h"
#include "test/TestFixtureSupport.h"
#include "test/library/MusicLibraryTestSupport.h"
#include "test/library/WritableLibraryTestSupport.h"
#include "test/lmdb/LmdbTestSupport.h"
#include <ao/AudioScalars.h>
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/PictureType.h>
#include <ao/library/DictionaryStore.h>
#include <ao/library/FileManifestBuilder.h>
#include <ao/library/FileManifestLayout.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/LibraryWrite.h>
#include <ao/library/ListBuilder.h>
#include <ao/library/ListStore.h>
#include <ao/library/ListWriter.h>
#include <ao/library/ResourceStore.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackLayout.h>
#include <ao/library/TrackStore.h>
#include <ao/library/TrackWriter.h>
#include <ao/lmdb/Database.h>
#include <ao/lmdb/Environment.h>
#include <ao/lmdb/Transaction.h>
#include <ao/utility/ByteView.h>

#include <catch2/catch_test_macros.hpp>
#include <lmdb.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ao::library::test
{
  namespace
  {
    TrackBuilder segmentTrack(std::uint16_t ordinal, std::uint64_t start, std::uint64_t count)
    {
      auto builder = TrackBuilder::makeEmpty();
      builder.metadata().title("Segment");
      builder.property()
        .uri("disc.flac")
        .sampleRate(SampleRate{44100})
        .segment(TrackSegment{.startFrame = start, .frameCount = count, .ordinal = ordinal});
      return builder;
    }

    std::vector<TrackId> createCarrier(MusicLibrary& library)
    {
      // Input order differs from the manifest's ordinal order; the gap is legal.
      auto tracks = std::array{segmentTrack(9, 1000, 588), segmentTrack(2, 0, 588)};
      tracks[0].property().duration(std::chrono::hours{24});
      auto transaction = writeTransaction(library);
      auto idsRes = transaction.apply(
        [&](LibraryWrite& write)
        { return write.tracks().createCarrier(tracks, FileManifestBuilder::makeEmpty().fileSize(12345), 2000); });
      REQUIRE(idsRes);
      REQUIRE(transaction.commit());
      return *idsRes;
    }

    template<typename Function>
    void changeManifest(std::filesystem::path const& path, Function function)
    {
      auto environment = lmdb::test::openEnvironment(path, {.flags = MDB_NOTLS, .maxDatabases = 8});
      auto transaction = lmdb::test::beginWriteTransaction(environment);
      auto database = lmdb::test::openByteKeyDatabase(transaction, "file_manifest");
      auto writer = database.writer(transaction);
      auto const key = detail::PaddedFileManifestKey{"disc.flac"};
      auto const optBytes = writer.get(key.bytes());
      REQUIRE(optBytes);
      auto bytes = std::vector<std::byte>{optBytes->begin(), optBytes->end()};
      function(bytes);
      REQUIRE(writer.update(key.bytes(), bytes));
      REQUIRE(transaction.commit());
    }

    template<typename Function>
    void changeTrack(std::filesystem::path const& path, std::string_view databaseName, TrackId id, Function function)
    {
      auto environment = lmdb::test::openEnvironment(path, {.flags = MDB_NOTLS, .maxDatabases = 8});
      auto transaction = lmdb::test::beginWriteTransaction(environment);
      auto database = lmdb::test::openIntegerKeyDatabase(transaction, std::string{databaseName});
      auto writer = database.writer(transaction);
      auto const optBytes = writer.get(id.raw());
      REQUIRE(optBytes);
      auto bytes = std::vector<std::byte>{optBytes->begin(), optBytes->end()};
      function(bytes);
      REQUIRE(writer.update(id.raw(), bytes));
      REQUIRE(transaction.commit());
    }

    TrackSegmentBlock& segmentBlock(std::vector<std::byte>& bytes)
    {
      auto const* header = utility::layout::view<TrackColdHeader>(bytes);
      return *utility::layout::viewMutable<TrackSegmentBlock>(
        std::span{bytes}.subspan(header->blockOffsets[trackColdBlockSlotIndex(TrackColdBlockSlot::Segment)]));
    }
  } // namespace

  TEST_CASE("Carrier Track - sparse members derive duration and preserve identity through append relink and deletion")
  {
    auto const temp = ao::test::TempDir{};
    auto ids = std::vector<TrackId>{};
    auto listId = kInvalidListId;

    {
      auto library = makeTestMusicLibrary(temp.path(), temp.path() / "db");
      ids = createCarrier(library);
      auto transaction = writeTransaction(library);
      REQUIRE(transaction.apply(
        [&](LibraryWrite& write) -> Result<>
        {
          auto writer = write.tracks();
          auto const manifest = *writer.manifest("disc.flac");
          CHECK(manifest.trackId() == kInvalidTrackId);
          CHECK(manifest.segmentCount() == 2);
          CHECK(manifest.totalFrames() == 2000);
          CHECK(std::ranges::equal(manifest.segmentTrackIds(), std::array{ids[1], ids[0]}));
          auto const view = *writer.get(ids[0]);
          CHECK(view.property().segment() == TrackSegment{.startFrame = 1000, .frameCount = 588, .ordinal = 9});
          CHECK(view.property().duration() == std::chrono::milliseconds{13});
          auto builder = TrackBuilder::fromCompleteView(view, library.dictionary());
          builder.metadata().title("Refreshed");
          builder.property().bitrate(Bitrate{1000}).duration(std::chrono::seconds{123});
          auto const coverBytes = std::array{std::byte{1}, std::byte{2}, std::byte{3}};
          builder.coverArt().add(PictureType::FrontCover, std::span<std::byte const>{coverBytes});
          REQUIRE(writer.update(ids[0], builder));
          auto append = std::array{segmentTrack(5, 600, 300)};
          auto appendedRes = writer.addCarrierMembers("disc.flac", append);
          REQUIRE(appendedRes);
          ids.push_back(appendedRes->front());
          auto list = ListBuilder::makeEmpty();
          list.name("Carrier order");
          list.orderTrackIds().add(ids[0]).add(ids[1]).add(ids[2]);
          auto listRes = write.lists().create(list);
          REQUIRE(listRes);
          listId = *listRes;
          REQUIRE(writer.relinkCarrier("disc.flac", "moved.flac", FileManifestBuilder::makeEmpty().fileSize(54321)));
          REQUIRE(writer.remove(ids[1]));
          return {};
        }));
      REQUIRE(transaction.commit());
    }

    {
      auto library = makeTestMusicLibrary(temp.path(), temp.path() / "db");
      auto transaction = writeTransaction(library);
      REQUIRE(transaction.apply(
        [&](LibraryWrite& write) -> Result<>
        {
          auto writer = write.tracks();
          CHECK_FALSE(writer.manifest("disc.flac"));
          auto const manifest = *writer.manifest("moved.flac");
          CHECK(manifest.totalFrames() == 2000);
          CHECK(manifest.fileSize() == 54321);
          CHECK(std::ranges::equal(manifest.segmentTrackIds(), std::array{ids[2], ids[0]}));
          auto const view = *writer.get(ids[0]);
          CHECK(view.property().uri() == "moved.flac");
          CHECK(view.metadata().title() == "Refreshed");
          REQUIRE(view.coverArt().primary());
          CHECK(view.coverArt().primary()->type == PictureType::FrontCover);
          CHECK(view.property().sampleRate() == SampleRate{44100});
          CHECK(view.property().duration() == std::chrono::milliseconds{13});
          CHECK(view.property().segment()->ordinal == 9);
          // List order belongs to runtime commands, which publish its change.
          CHECK(std::ranges::equal(write.lists().get(listId)->orderTrackIds(), std::array{ids[0], ids[1], ids[2]}));
          REQUIRE(writer.remove(ids[2]));
          CHECK(writer.manifest("moved.flac")->segmentCount() == 1);
          REQUIRE(writer.updateManifest("moved.flac", FileManifestBuilder::makeEmpty().status(FileStatus::Missing)));
          CHECK(writer.manifest("moved.flac")->status() == FileStatus::Missing);
          REQUIRE(writer.remove(ids[0]));
          CHECK_FALSE(writer.manifest("moved.flac"));
          return {};
        }));
      REQUIRE(transaction.commit());
    }

    auto library = makeTestMusicLibrary(temp.path(), temp.path() / "db");
    auto read = library.readTransaction();
    CHECK(library.tracks().reader(read).entryCount() == 0);
  }

  TEST_CASE("Carrier Track - aggregate preflight rejects invalid final layouts without writing")
  {
    auto const temp = ao::test::TempDir{};
    auto library = makeTestMusicLibrary(temp.path(), temp.path() / "db");
    auto tracks = std::array{segmentTrack(2, 0, 588), segmentTrack(9, 1000, 588)};

    SECTION("overlap")
    {
      tracks[1].property().segment(TrackSegment{.startFrame = 587, .frameCount = 588, .ordinal = 9});
    }

    SECTION("backwards ordinal ranges")
    {
      tracks[1].property().segment(TrackSegment{.startFrame = 1000, .frameCount = 588, .ordinal = 1});
    }

    SECTION("duplicate ordinal")
    {
      tracks[1].property().segment(TrackSegment{.startFrame = 1000, .frameCount = 588, .ordinal = 2});
    }

    SECTION("zero ordinal")
    {
      tracks[1].property().segment(TrackSegment{.startFrame = 1000, .frameCount = 588, .ordinal = 0});
    }

    SECTION("ordinal beyond CUE range")
    {
      tracks[1].property().segment(TrackSegment{.startFrame = 1000, .frameCount = 588, .ordinal = 100});
    }

    SECTION("empty range")
    {
      tracks[1].property().segment(TrackSegment{.startFrame = 1000, .frameCount = 0, .ordinal = 9});
    }

    SECTION("overflowing range")
    {
      tracks[1].property().segment(TrackSegment{.startFrame = UINT64_MAX, .frameCount = 588, .ordinal = 9});
    }

    SECTION("extent exceeded")
    {
      tracks[1].property().segment(TrackSegment{.startFrame = 1600, .frameCount = 588, .ordinal = 9});
    }

    SECTION("different URI")
    {
      tracks[1].property().uri("another.flac");
    }

    SECTION("different sample rate")
    {
      tracks[1].property().sampleRate(SampleRate{48000});
    }

    SECTION("zero sample rate")
    {
      tracks[1].property().sampleRate(SampleRate{});
    }

    SECTION("whole member")
    {
      tracks[1].property().segment(std::nullopt);
    }

    auto transaction = writeTransaction(library);
    auto res = transaction.apply(
      [&](LibraryWrite& write) -> Result<>
      {
        auto writer = write.tracks();
        auto rejectedRes = writer.createCarrier(tracks, FileManifestBuilder::makeEmpty(), 2000);
        REQUIRE_FALSE(rejectedRes);
        CHECK(rejectedRes.error().code == Error::Code::InvalidInput);
        // A preflight rejection has staged nothing, even if its caller handles it.
        CHECK_FALSE(writer.manifest("disc.flac"));
        auto valid = std::array{segmentTrack(3, 0, 588)};
        auto validRes = writer.createCarrier(valid, FileManifestBuilder::makeEmpty(), 2000);
        REQUIRE(validRes);
        CHECK(validRes->front() == TrackId{1});
        return {};
      });
    REQUIRE(res);
    REQUIRE(transaction.commit());
    auto read = library.readTransaction();
    CHECK(library.tracks().reader(read).entryCount() == 1);
    CHECK(library.manifest().reader(read).get("disc.flac")->segmentCount() == 1);
  }

  TEST_CASE("Carrier Track - every ordinary mutation rejects segment structure bypasses")
  {
    auto const temp = ao::test::TempDir{};
    auto library = makeTestMusicLibrary(temp.path(), temp.path() / "db");
    auto const ids = createCarrier(library);
    auto transaction = writeTransaction(library);
    auto res = transaction.apply(
      [&](LibraryWrite& write) -> Result<>
      {
        auto writer = write.tracks();
        auto builder = TrackBuilder::fromCompleteView(*writer.get(ids[0]), library.dictionary());

        SECTION("complete update removes segment")
        {
          builder.property().segment(std::nullopt);
          return writer.update(ids[0], builder);
        }

        SECTION("cold update changes ordinal")
        {
          builder.property().segment(TrackSegment{.startFrame = 1000, .frameCount = 588, .ordinal = 10});
          return writer.updateCold(ids[0], builder);
        }

        SECTION("cold update changes rate")
        {
          builder.property().sampleRate(SampleRate{48000});
          return writer.updateCold(ids[0], builder);
        }

        SECTION("hot update changes rate")
        {
          builder.property().sampleRate(SampleRate{48000});
          return writer.updateHot(ids[0], builder);
        }

        SECTION("hot-only update changes rate")
        {
          builder = TrackBuilder::fromHotView(*writer.get(ids[0]), library.dictionary());
          builder.property().sampleRate(SampleRate{48000});
          return writer.updateHot(ids[0], builder);
        }

        SECTION("replace converts segment to whole")
        {
          builder.property().segment(std::nullopt);
          return writer.replace(ids[0], builder, FileManifestBuilder::makeEmpty());
        }

        SECTION("replace changes URI")
        {
          builder.property().uri("other.flac");
          return writer.replace(ids[0], builder, FileManifestBuilder::makeEmpty());
        }

        SECTION("single relink")
        {
          builder.property().uri("other.flac");
          return writer.relink(ids[0], builder, FileManifestBuilder::makeEmpty());
        }

        SECTION("single create")
        {
          auto createRes = writer.create(builder, FileManifestBuilder::makeEmpty());
          REQUIRE_FALSE(createRes);
          return std::unexpected{createRes.error()};
        }

        return makeError(Error::Code::InvalidInput, "No section selected");
      });
    REQUIRE_FALSE(res);
    CHECK(res.error().code == Error::Code::InvalidInput);
    auto read = library.readTransaction();
    auto const view = *library.tracks().reader(read).get(ids[0]);
    CHECK(view.property().segment() == TrackSegment{.startFrame = 1000, .frameCount = 588, .ordinal = 9});
    CHECK(view.property().sampleRate() == SampleRate{44100});
    CHECK(view.property().duration() == std::chrono::milliseconds{13});
    CHECK(view.property().uri() == "disc.flac");
    CHECK(library.manifest().reader(read).get("disc.flac")->segmentCount() == 2);
  }

  TEST_CASE("Carrier Track - nonstructural updates preserve the aggregate across every entry point")
  {
    auto const temp = ao::test::TempDir{};
    auto ids = std::vector<TrackId>{};
    {
      auto library = makeTestMusicLibrary(temp.path(), temp.path() / "db");
      ids = createCarrier(library);
      auto transaction = writeTransaction(library);
      REQUIRE(transaction.apply(
        [&](LibraryWrite& write) -> Result<>
        {
          auto writer = write.tracks();
          auto hot = TrackBuilder::fromHotView(*writer.get(ids[0]), library.dictionary());
          hot.metadata().title("Hot refresh");
          REQUIRE(writer.updateHot(ids[0], hot));
          auto complete = TrackBuilder::fromCompleteView(*writer.get(ids[0]), library.dictionary());
          complete.property().bitrate(Bitrate{2345});
          REQUIRE(writer.updateCold(ids[0], complete));
          // Rebuild after each mutation: builders borrow mapped URI/metadata bytes.
          complete = TrackBuilder::fromCompleteView(*writer.get(ids[0]), library.dictionary());
          complete.metadata().title("Replacement");
          REQUIRE(writer.replace(ids[0], complete, FileManifestBuilder::makeEmpty().fileSize(9876)));
          REQUIRE(writer.updateManifest(ids[1], FileManifestBuilder::makeEmpty().status(FileStatus::Missing)));
          return {};
        }));
      REQUIRE(transaction.commit());
    }
    auto library = makeTestMusicLibrary(temp.path(), temp.path() / "db");
    auto read = library.readTransaction();
    auto const view = *library.tracks().reader(read).get(ids[0]);
    CHECK(view.metadata().title() == "Replacement");
    CHECK(view.property().bitrate() == Bitrate{2345});
    CHECK(view.property().duration() == std::chrono::milliseconds{13});
    CHECK(view.property().segment() == TrackSegment{.startFrame = 1000, .frameCount = 588, .ordinal = 9});
    auto const manifest = *library.manifest().reader(read).get("disc.flac");
    CHECK(manifest.status() == FileStatus::Missing);
    CHECK(manifest.totalFrames() == 2000);
    CHECK(std::ranges::equal(manifest.segmentTrackIds(), std::array{ids[1], ids[0]}));
  }

  TEST_CASE("Carrier Track - whole file conflicts and conversions cannot bypass aggregate creation")
  {
    auto const temp = ao::test::TempDir{};
    auto library = makeTestMusicLibrary(temp.path(), temp.path() / "db");
    auto transaction = writeTransaction(library);
    REQUIRE(transaction.apply(
      [&](LibraryWrite& write) -> Result<>
      {
        auto writer = write.tracks();
        auto whole = TrackBuilder::makeEmpty();
        whole.property().uri("disc.flac").sampleRate(SampleRate{44100});
        auto createdRes = writer.create(whole, FileManifestBuilder::makeEmpty());
        REQUIRE(createdRes);
        auto const id = *createdRes;
        auto tracks = std::array{segmentTrack(1, 0, 588)};
        CHECK_FALSE(writer.createCarrier(tracks, FileManifestBuilder::makeEmpty(), 2000));
        CHECK_FALSE(writer.addCarrierMembers("disc.flac", tracks));
        CHECK_FALSE(writer.update(id, tracks[0]));
        CHECK_FALSE(writer.updateCold(id, tracks[0]));
        CHECK_FALSE(writer.updateHot(id, tracks[0]));
        CHECK_FALSE(writer.replace(id, tracks[0], FileManifestBuilder::makeEmpty()));
        CHECK_FALSE(writer.relink(id, tracks[0], FileManifestBuilder::makeEmpty()));
        CHECK_FALSE(writer.relinkCarrier("disc.flac", "other.flac", FileManifestBuilder::makeEmpty()));
        CHECK_FALSE(writer.get(id)->property().segment());
        CHECK(writer.manifest("disc.flac")->trackId() == id);
        return {};
      }));
    REQUIRE(transaction.commit());
    auto read = library.readTransaction();
    CHECK(library.tracks().reader(read).entryCount() == 1);
    CHECK(library.manifest().reader(read).get("disc.flac")->segmentCount() == 0);
  }

  TEST_CASE("Carrier Track - relink onto a taken URI conflicts and noncanonical URIs are input errors")
  {
    auto const temp = ao::test::TempDir{};
    auto library = makeTestMusicLibrary(temp.path(), temp.path() / "db");
    auto const ids = createCarrier(library);
    auto transaction = writeTransaction(library);
    REQUIRE(transaction.apply(
      [&](LibraryWrite& write) -> Result<>
      {
        auto writer = write.tracks();
        auto whole = TrackBuilder::makeEmpty();
        whole.property().uri("taken.flac").sampleRate(SampleRate{44100});
        REQUIRE(writer.create(whole, FileManifestBuilder::makeEmpty()));
        auto const conflictRes = writer.relinkCarrier("disc.flac", "taken.flac", FileManifestBuilder::makeEmpty());
        REQUIRE_FALSE(conflictRes);
        CHECK(conflictRes.error().code == Error::Code::Conflict);

        // Manifest lookup treats such text as a broken precondition, so each
        // carrier entry point must reject it before looking anything up.
        auto const append = std::array{segmentTrack(5, 600, 300)};

        for (auto const uri : std::to_array<std::string_view>({"./disc.flac", "disc.flac/", "sub\\disc.flac"}))
        {
          auto const addRes = writer.addCarrierMembers(uri, append);
          REQUIRE_FALSE(addRes);
          CHECK(addRes.error().code == Error::Code::InvalidInput);
          auto const relinkRes = writer.relinkCarrier(uri, "moved.flac", FileManifestBuilder::makeEmpty());
          REQUIRE_FALSE(relinkRes);
          CHECK(relinkRes.error().code == Error::Code::InvalidInput);
          auto const updateRes = writer.updateManifest(uri, FileManifestBuilder::makeEmpty());
          REQUIRE_FALSE(updateRes);
          CHECK(updateRes.error().code == Error::Code::InvalidInput);
        }

        CHECK(writer.manifest("disc.flac")->segmentCount() == 2);
        CHECK(writer.get(ids[0])->property().uri() == "disc.flac");
        CHECK(writer.manifest("taken.flac")->segmentCount() == 0);
        return {};
      }));
  }

  TEST_CASE("Carrier Track - a later root failure rolls back aggregate membership relink and removal")
  {
    auto const temp = ao::test::TempDir{};
    auto library = makeTestMusicLibrary(temp.path(), temp.path() / "db");
    auto const ids = createCarrier(library);
    auto listId = kInvalidListId;
    {
      auto transaction = writeTransaction(library);
      REQUIRE(transaction.apply(
        [&](LibraryWrite& write) -> Result<>
        {
          auto list = ListBuilder::makeEmpty();
          list.name("Preserved");
          list.orderTrackIds().add(ids[0]).add(ids[1]);
          auto createdRes = write.lists().create(list);
          REQUIRE(createdRes);
          listId = *createdRes;
          return {};
        }));
      REQUIRE(transaction.commit());
    }
    auto transaction = writeTransaction(library);
    auto res = transaction.apply(
      [&](LibraryWrite& write) -> Result<>
      {
        auto writer = write.tracks();
        auto append = std::array{segmentTrack(5, 600, 300)};
        REQUIRE(writer.addCarrierMembers("disc.flac", append));
        REQUIRE(writer.relinkCarrier("disc.flac", "rolled-back.flac", FileManifestBuilder::makeEmpty()));
        REQUIRE(writer.remove(ids[0]));
        return makeError(Error::Code::InvalidInput, "Rollback aggregate operations");
      });
    REQUIRE_FALSE(res);
    auto read = library.readTransaction();
    CHECK(library.tracks().reader(read).entryCount() == 2);
    CHECK_FALSE(library.manifest().reader(read).get("rolled-back.flac"));
    CHECK(std::ranges::equal(
      library.manifest().reader(read).get("disc.flac")->segmentTrackIds(), std::array{ids[1], ids[0]}));
    CHECK(std::ranges::equal(library.lists().reader(read).get(listId)->orderTrackIds(), std::array{ids[0], ids[1]}));
  }

  TEST_CASE("Carrier Track - handled append rejection stages nothing before a successful append commits")
  {
    auto const temp = ao::test::TempDir{};
    auto library = makeTestMusicLibrary(temp.path(), temp.path() / "db");
    auto const ids = createCarrier(library);
    auto append = std::array{segmentTrack(5, 600, 300)};

    SECTION("overlap existing predecessor")
    {
      append[0] = segmentTrack(5, 500, 300);
    }

    SECTION("overlap existing successor")
    {
      append[0] = segmentTrack(5, 600, 401);
    }

    SECTION("outside extent")
    {
      append[0] = segmentTrack(10, 1900, 101);
    }

    SECTION("duplicate existing ordinal")
    {
      append[0] = segmentTrack(2, 600, 300);
    }

    SECTION("different rate")
    {
      append[0].property().sampleRate(SampleRate{48000});
    }

    auto const dictionarySize = library.dictionary().size();
    auto const coverBytes = std::array{std::byte{0x41}, std::byte{0x42}};
    append[0].metadata().artist("Rejected append artist");
    append[0].coverArt().add(PictureType::FrontCover, std::span<std::byte const>{coverBytes});

    auto transaction = writeTransaction(library);
    auto res = transaction.apply(
      [&](LibraryWrite& write) -> Result<std::vector<TrackId>>
      {
        auto writer = write.tracks();
        auto rejectedRes = writer.addCarrierMembers("disc.flac", append);
        REQUIRE_FALSE(rejectedRes);
        CHECK(rejectedRes.error().code == Error::Code::InvalidInput);
        auto const optManifest = writer.manifest("disc.flac");
        REQUIRE(optManifest);
        CHECK(std::ranges::equal(optManifest->segmentTrackIds(), std::array{ids[1], ids[0]}));
        // Handle the rejection rather than aborting the root: a later commit
        // must not publish dictionary, Resource or id effects from preflight.
        auto const valid = std::array{segmentTrack(5, 600, 300)};
        return writer.addCarrierMembers("disc.flac", valid);
      });
    REQUIRE(res);
    REQUIRE(res->size() == 1);
    CHECK(res->front() == TrackId{3});
    REQUIRE(transaction.commit());

    auto read = library.readTransaction();
    auto const reader = library.tracks().reader(read);
    CHECK(reader.entryCount() == 3);
    auto const optManifest = library.manifest().reader(read).get("disc.flac");
    REQUIRE(optManifest);
    CHECK(std::ranges::equal(optManifest->segmentTrackIds(), std::array{ids[1], res->front(), ids[0]}));
    auto const optAdded = reader.get(res->front());
    REQUIRE(optAdded);
    CHECK(optAdded->property().segment() == TrackSegment{.startFrame = 600, .frameCount = 300, .ordinal = 5});
    CHECK(optAdded->coverArt().empty());
    CHECK(library.dictionary().size() == dictionarySize);
    CHECK_FALSE(library.dictionary().findId("Rejected append artist"));
    auto const resources = library.resources().reader(read);
    CHECK(resources.begin() == resources.end());
  }

  TEST_CASE("Carrier Track - open rejects malformed persisted carrier structures")
  {
    auto const temp = ao::test::TempDir{};
    auto ids = std::vector<TrackId>{};
    auto const path = temp.path() / "db";
    {
      auto library = makeTestMusicLibrary(temp.path(), path);
      ids = createCarrier(library);
    }

    SECTION("nonzero reserved")
    {
      changeTrack(path, "tracks_cold", ids[0], [](auto& bytes) { segmentBlock(bytes).reserved = 1; });
    }

    SECTION("ordinal zero")
    {
      changeTrack(path, "tracks_cold", ids[0], [](auto& bytes) { segmentBlock(bytes).ordinal = 0; });
    }

    SECTION("ordinal too large")
    {
      changeTrack(path, "tracks_cold", ids[0], [](auto& bytes) { segmentBlock(bytes).ordinal = 100; });
    }

    SECTION("zero frame count")
    {
      changeTrack(path, "tracks_cold", ids[0], [](auto& bytes) { segmentBlock(bytes).frameCountLo = 0; });
    }

    SECTION("range overflow")
    {
      changeTrack(path,
                  "tracks_cold",
                  ids[0],
                  [](auto& bytes)
                  {
                    auto& block = segmentBlock(bytes);
                    block.startFrameLo = UINT32_MAX;
                    block.startFrameHi = UINT32_MAX;
                  });
    }

    SECTION("outside extent")
    {
      changeManifest(path,
                     [](auto& bytes)
                     {
                       std::uint64_t frames = 1500;
                       std::memcpy(bytes.data() + 52, &frames, 8);
                     });
    }

    SECTION("overlap")
    {
      changeTrack(path, "tracks_cold", ids[0], [](auto& bytes) { segmentBlock(bytes).startFrameLo = 500; });
    }

    SECTION("reversed ordinals")
    {
      changeTrack(path, "tracks_cold", ids[0], [](auto& bytes) { segmentBlock(bytes).ordinal = 1; });
    }

    SECTION("duplicate ordinals")
    {
      changeTrack(path, "tracks_cold", ids[0], [](auto& bytes) { segmentBlock(bytes).ordinal = 2; });
    }

    SECTION("unequal sample rates with equal derived durations")
    {
      changeTrack(path,
                  "tracks_hot",
                  ids[0],
                  [](auto& bytes)
                  { utility::layout::viewMutable<TrackHotHeader>(bytes)->sampleRate = SampleRate{45000}; });
    }

    SECTION("zero sample rate")
    {
      changeTrack(path,
                  "tracks_hot",
                  ids[0],
                  [](auto& bytes) { utility::layout::viewMutable<TrackHotHeader>(bytes)->sampleRate = SampleRate{}; });
    }

    SECTION("contradictory duration")
    {
      changeTrack(path,
                  "tracks_cold",
                  ids[0],
                  [](auto& bytes)
                  { utility::layout::viewMutable<TrackColdHeader>(bytes)->duration = TrackDuration{14}; });
    }

    SECTION("missing segment block")
    {
      changeTrack(path,
                  "tracks_cold",
                  ids[0],
                  [](auto& bytes) { utility::layout::viewMutable<TrackColdHeader>(bytes)->blockOffsets[4] = 0; });
    }

    SECTION("mismatched URI")
    {
      changeTrack(path,
                  "tracks_cold",
                  ids[0],
                  [](auto& bytes)
                  {
                    auto const* header = utility::layout::view<TrackColdHeader>(bytes);
                    bytes[header->uriOffset] = std::byte{'x'};
                  });
    }

    SECTION("header Track id in carrier")
    {
      changeManifest(
        path, [](auto& bytes) { utility::layout::viewMutable<FileManifestHeader>(bytes)->trackId = TrackId{1}; });
    }

    SECTION("missing member")
    {
      changeManifest(path,
                     [](auto& bytes)
                     {
                       auto id = TrackId{100};
                       std::memcpy(bytes.data() + 60, &id, 4);
                     });
    }

    SECTION("duplicate member")
    {
      changeManifest(path, [](auto& bytes) { std::memcpy(bytes.data() + 64, bytes.data() + 60, 4); });
    }

    SECTION("reversed members")
    {
      changeManifest(path,
                     [](auto& bytes)
                     {
                       auto* ids = utility::layout::viewArrayMutable<TrackId>(std::span{bytes}.subspan(60)).data();
                       std::swap(ids[0], ids[1]);
                     });
    }

    SECTION("zero extent")
    {
      changeManifest(path, [](auto& bytes) { std::fill(bytes.begin() + 52, bytes.begin() + 60, std::byte{0}); });
    }

    SECTION("zero member")
    {
      changeManifest(path, [](auto& bytes) { std::fill(bytes.begin() + 60, bytes.begin() + 64, std::byte{0}); });
    }

    SECTION("too many members")
    {
      changeManifest(
        path, [](auto& bytes) { utility::layout::viewMutable<FileManifestHeader>(bytes)->segmentCount = 100; });
    }

    SECTION("truncated tail")
    {
      changeManifest(path, [](auto& bytes) { bytes.pop_back(); });
    }

    SECTION("trailing bytes")
    {
      changeManifest(path, [](auto& bytes) { bytes.push_back(std::byte{0}); });
    }

    SECTION("whole row with carrier tail")
    {
      changeManifest(path,
                     [](auto& bytes)
                     {
                       auto* header = utility::layout::viewMutable<FileManifestHeader>(bytes);
                       header->segmentCount = 0;
                       header->trackId = TrackId{1};
                     });
    }

    auto openRes = openTestMusicLibrary(temp.path(), path);
    REQUIRE_FALSE(openRes);
    CHECK(openRes.error().code == Error::Code::CorruptData);
  }

  TEST_CASE("Carrier Track - high frame words and zero millisecond members survive reopen")
  {
    auto const temp = ao::test::TempDir{};
    auto ids = std::vector<TrackId>{};
    {
      auto library = makeTestMusicLibrary(temp.path(), temp.path() / "db");
      auto tracks = std::array{segmentTrack(1, 0, 1), segmentTrack(99, 0x100000000ULL, 0x10000024cULL)};
      tracks[0].property().sampleRate(SampleRate{192000});
      tracks[1].property().sampleRate(SampleRate{192000});
      auto transaction = writeTransaction(library);
      auto idsRes = transaction.apply(
        [&](LibraryWrite& write)
        { return write.tracks().createCarrier(tracks, FileManifestBuilder::makeEmpty(), 0x20000024cULL); });
      REQUIRE(idsRes);
      ids = *idsRes;
      REQUIRE(transaction.commit());
    }
    auto library = makeTestMusicLibrary(temp.path(), temp.path() / "db");
    auto read = library.readTransaction();
    auto const reader = library.tracks().reader(read);
    CHECK(reader.get(ids[0])->property().duration() == std::chrono::milliseconds{0});
    auto const second = *reader.get(ids[1]);
    CHECK(second.property().segment() ==
          TrackSegment{.startFrame = 0x100000000ULL, .frameCount = 0x10000024cULL, .ordinal = 99});
    CHECK(second.property().duration() == std::chrono::milliseconds{22369624});
    CHECK(library.manifest().reader(read).get("disc.flac")->totalFrames() == 0x20000024cULL);
  }

  TEST_CASE("Carrier Track - frame duration and manifest references use exact arithmetic and four byte alignment")
  {
    CHECK(sizeof(TrackSegmentBlock) == 20);
    CHECK(alignof(TrackSegmentBlock) == 4);
    CHECK(offsetof(FileManifestHeader, segmentCount) == 50);
    CHECK(segmentDuration(588, SampleRate{44100}) == TrackDuration{13});
    CHECK(segmentDuration(44, SampleRate{44100}) == TrackDuration{0});
    CHECK_FALSE(segmentDuration(UINT64_MAX, SampleRate{1}));
    CHECK_FALSE(segmentDuration(588, SampleRate{}));
    alignas(8) auto storage = std::array<std::byte, 68>{};
    auto bytes = std::span{storage}.subspan(4);
    auto* header = utility::layout::viewMutable<FileManifestHeader>(bytes);
    header->segmentCount = 1;
    std::uint64_t frames = 1234567890123;
    auto id = TrackId{42};
    std::memcpy(bytes.data() + 52, &frames, 8);
    std::memcpy(bytes.data() + 60, &id, 4);
    REQUIRE(validateFileManifestPayload(bytes));
    auto view = FileManifestView{bytes};
    REQUIRE(view.isValid());
    CHECK(view.totalFrames() == frames);
    REQUIRE(view.segmentTrackIds().size() == 1);
    CHECK(view.segmentTrackIds().front() == id);
  }
} // namespace ao::library::test
