// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/library/TrackWriter.h>

#include "test/unit/TestFixtureSupport.h"
#include "test/unit/library/MusicLibraryTestSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/library/WritableLibraryTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/PictureType.h>
#include <ao/library/DictionaryStore.h>
#include <ao/library/FileManifestBuilder.h>
#include <ao/library/FileManifestLayout.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/LibraryWrite.h>
#include <ao/library/ListStore.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/ResourceStore.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackStore.h>
#include <ao/library/WriteTransaction.h>
#include <ao/utility/Sha256.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <span>
#include <type_traits>

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

    TrackBuilder builderFrom(MusicLibrary& library, TrackWriter const& writer, TrackId const id)
    {
      auto const optView = writer.get(id, TrackStore::Reader::LoadMode::Both);
      REQUIRE(optView);
      return TrackBuilder::fromCompleteView(*optView, library.dictionary());
    }
  } // namespace

  TEST_CASE("TrackWriter - create commits one Track, manifest, dictionary, and Resource graph",
            "[library][unit][track-writer]")
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
    auto manifest = FileManifestBuilder::makeEmpty();
    manifest.fileSize(321).mtime(654).status(FileStatus::Available);

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
    CHECK(optManifest->mtime() == 654);
  }

  TEST_CASE("TrackWriter - create rejects a missing existing Resource before mutation",
            "[library][regression][track-writer]")
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

  TEST_CASE("TrackWriter - create rejects a duplicate manifest URI without aliasing it",
            "[library][regression][track-writer]")
  {
    auto const temp = ao::test::TempDir{};
    auto library = makeTestMusicLibrary(temp.path(), temp.path() / "db");
    [[maybe_unused]] auto const firstId = addTrack(library, TrackSpec{.title = "First", .uri = "shared.flac"});
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
  }

  TEST_CASE("TrackWriter - validate accepts complete builders and rejects hot-only builders as input",
            "[library][unit][track-writer]")
  {
    auto const temp = ao::test::TempDir{};
    auto library = makeTestMusicLibrary(temp.path(), temp.path() / "db");
    auto const trackId = addTrack(library, TrackSpec{.title = "Stored", .uri = "stored.flac"});
    auto transaction = writeTransaction(library);

    REQUIRE(transaction.apply(
      [&](LibraryWrite& write) -> Result<>
      {
        auto writer = write.tracks();
        auto const optComplete = writer.get(trackId, TrackStore::Reader::LoadMode::Both);
        REQUIRE(optComplete);
        auto complete = TrackBuilder::fromCompleteView(*optComplete, library.dictionary());
        REQUIRE(writer.validate(complete));

        auto const optHot = writer.get(trackId, TrackStore::Reader::LoadMode::Hot);
        REQUIRE(optHot);
        auto hotOnly = TrackBuilder::fromHotView(*optHot, library.dictionary());
        auto hotOnlyRes = writer.validate(hotOnly);
        REQUIRE_FALSE(hotOnlyRes);
        CHECK(hotOnlyRes.error().code == Error::Code::InvalidInput);
        return {};
      }));
    REQUIRE(transaction.commit());
  }

  TEST_CASE("TrackWriter - full, cold, and replacement updates persist their owned sides",
            "[library][unit][track-writer]")
  {
    auto const temp = ao::test::TempDir{};
    auto library = makeTestMusicLibrary(temp.path(), temp.path() / "db");
    auto const trackId = addTrack(library, TrackSpec{.title = "Before", .uri = "updates.flac"});
    auto transaction = writeTransaction(library);

    REQUIRE(transaction.apply(
      [&](LibraryWrite& write) -> Result<>
      {
        auto writer = write.tracks();
        auto full = builderFrom(library, writer, trackId);
        full.metadata().title("Full update");
        full.property().duration(std::chrono::seconds{210});
        REQUIRE(writer.update(trackId, full));

        auto cold = builderFrom(library, writer, trackId);
        cold.metadata().title("Ignored by cold update");
        cold.property().duration(std::chrono::seconds{220});
        REQUIRE(writer.updateCold(trackId, cold));

        auto replacement = builderFrom(library, writer, trackId);
        replacement.metadata().title("Replacement");
        replacement.property().duration(std::chrono::seconds{230});
        auto const optManifest = writer.manifest("updates.flac");
        REQUIRE(optManifest);
        auto manifest = FileManifestBuilder::fromView(*optManifest);
        manifest.fileSize(987).status(FileStatus::Missing);
        return writer.replace(trackId, replacement, manifest);
      }));
    REQUIRE(transaction.commit());

    auto readTransaction = library.readTransaction();
    auto const optTrack = library.tracks().reader(readTransaction).get(trackId, TrackStore::Reader::LoadMode::Both);
    REQUIRE(optTrack);
    CHECK(optTrack->metadata().title() == "Replacement");
    CHECK(optTrack->property().duration() == std::chrono::seconds{230});
    auto const optManifest = library.manifest().reader(readTransaction).get("updates.flac");
    REQUIRE(optManifest);
    CHECK(optManifest->trackId() == trackId);
    CHECK(optManifest->fileSize() == 987);
    CHECK(optManifest->status() == FileStatus::Missing);
  }

  TEST_CASE("TrackWriter - ordinary updates preserve the URI and manifest binding", "[library][unit][track-writer]")
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
          auto track = builderFrom(library, writer, trackId);
          track.metadata().title("After");

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

  TEST_CASE("TrackWriter - normal update rejects a URI change without committing partial state",
            "[library][regression][track-writer]")
  {
    auto const temp = ao::test::TempDir{};
    auto library = makeTestMusicLibrary(temp.path(), temp.path() / "db");
    auto const trackId = addTrack(library, TrackSpec{.title = "Before", .uri = "before.flac"});
    auto transaction = writeTransaction(library);

    auto updateRes = transaction.apply(
      [&](LibraryWrite& write)
      {
        auto writer = write.tracks();
        auto track = builderFrom(library, writer, trackId);
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

  TEST_CASE("TrackWriter - relink replaces the manifest key while preserving Track identity",
            "[library][unit][track-writer]")
  {
    auto const temp = ao::test::TempDir{};
    auto library = makeTestMusicLibrary(temp.path(), temp.path() / "db");
    auto const trackId = addTrack(library, TrackSpec{.title = "Moved", .uri = "old.flac"});
    auto transaction = writeTransaction(library);
    REQUIRE(transaction.apply(
      [&](LibraryWrite& write) -> Result<>
      {
        auto writer = write.tracks();
        auto track = builderFrom(library, writer, trackId);
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

  TEST_CASE("TrackWriter - delete and clear remove both Track and manifest sides", "[library][unit][track-writer]")
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

    {
      auto readTransaction = library.readTransaction();
      CHECK_FALSE(library.tracks().reader(readTransaction).get(firstId));
      CHECK_FALSE(library.manifest().reader(readTransaction).get("first.flac"));
      REQUIRE(library.tracks().reader(readTransaction).get(secondId));
      REQUIRE(library.manifest().reader(readTransaction).get("second.flac"));
    }

    auto transaction = writeTransaction(library);
    REQUIRE(transaction.apply([](LibraryWrite& write) { return write.tracks().clear(); }));
    REQUIRE(transaction.commit());

    auto readTransaction = library.readTransaction();
    CHECK(library.tracks().reader(readTransaction).entryCount() == 0);
    CHECK(library.manifest().reader(readTransaction).begin() == FileManifestStore::Reader::EndSentinel{});
  }

  TEST_CASE("TrackWriter - deleting an absent Track succeeds with false", "[library][unit][track-writer]")
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
