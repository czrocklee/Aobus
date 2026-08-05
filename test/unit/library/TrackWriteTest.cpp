// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/library/TrackWrite.h>

#include "lib/library/TrackRecordValidation.h"
#include "lib/lmdb/detail/TransactionFailure.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/library/MusicLibraryTestSupport.h"
#include "test/unit/library/TrackStoreTestSupport.h"
#include "test/unit/library/WritableLibraryTestSupport.h"
#include "test/unit/lmdb/LmdbTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/ResourceStore.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackStore.h>
#include <ao/library/WriteTransaction.h>

#include <catch2/catch_test_macros.hpp>
#include <lmdb.h>

#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace ao::library::test
{
  namespace
  {
    // Records reach storage only as prepared values, serialized straight into
    // storage-owned bytes. Reintroducing an entry point that takes caller-supplied
    // record bytes would bring back a second write path with its own copy and its
    // own validation, so the writer must keep rejecting one.
    template<typename Writer>
    concept HasRawRecordCreate =
      requires(Writer& writer) { writer.createHotCold(std::span<std::byte const>{}, std::span<std::byte const>{}); };

    template<typename Writer>
    concept HasRawRecordUpdate =
      requires(Writer& writer) { writer.updateHot(TrackId{1}, std::span<std::byte const>{}); };

    static_assert(!HasRawRecordCreate<TrackStore::Writer>);
    static_assert(!HasRawRecordUpdate<TrackStore::Writer>);
    static_assert(noexcept(std::declval<TrackBuilder::PreparedHot const&>().writeTo(std::span<std::byte>{})));
    static_assert(noexcept(std::declval<TrackBuilder::PreparedCold const&>().writeTo(std::span<std::byte>{})));

    void seedColdOnlyTrack(std::filesystem::path const& path)
    {
      initializeLibraryStorage(path);
      auto environment = lmdb::test::openEnvironment(path, {.flags = MDB_NOTLS, .maxDatabases = 8});
      auto transaction = lmdb::test::beginWriteTransaction(environment);
      std::ignore = lmdb::test::openDatabase(transaction, "tracks_hot");
      auto coldDatabase = lmdb::test::openDatabase(transaction, "tracks_cold");
      REQUIRE(coldDatabase.writer(transaction).create(1, makeColdData()));
      REQUIRE(transaction.commit());
    }

    std::pair<TrackBuilder::PreparedHot, TrackBuilder::PreparedCold> prepareTrack(TrackBuilder& builder,
                                                                                  WriteTransaction& transaction,
                                                                                  ResourceStore const& resources)
    {
      auto result = builder.prepare(transaction, resources);
      REQUIRE(result);
      return *std::move(result);
    }
  } // namespace

  TEST_CASE("createPreparedTrackRecord writes prepared hot and cold track records", "[library][unit][track]")
  {
    auto fixture = TrackStoreFixture{};
    auto transaction = writeTransaction(fixture.library);
    auto builder = TrackBuilder::makeEmpty();
    builder.metadata().title("Created Track").artist("Artist");
    builder.property().uri("created.flac");

    auto const [preparedHot, preparedCold] = prepareTrack(builder, transaction, fixture.library.resources());
    auto writer = fixture.store.writer(transaction);

    auto createResult = createPreparedTrackRecord(writer, preparedHot, preparedCold);
    REQUIRE(createResult);

    auto const trackId = *createResult;
    auto const optView = writer.get(trackId, TrackStore::Reader::LoadMode::Both);
    REQUIRE(optView);
    CHECK(trackId != kInvalidTrackId);
    CHECK(optView->metadata().title() == "Created Track");
    CHECK(optView->property().uri() == "created.flac");
    REQUIRE(transaction.commit());
  }

  TEST_CASE("TrackStore - prepared records emit canonical hot and cold bytes", "[library][unit][track-store]")
  {
    auto fixture = TrackStoreFixture{};
    auto transaction = writeTransaction(fixture.library);
    auto builder = TrackBuilder::makeEmpty();
    builder.metadata().title("Odd title").artist("Artist");
    builder.tags().add("favorite");
    builder.customMetadata().add("mood", "calm");
    builder.property().uri("odd.flac");
    auto const [preparedHot, preparedCold] = prepareTrack(builder, transaction, fixture.library.resources());
    auto hotBytes = std::vector<std::byte>(preparedHot.size());
    auto coldBytes = std::vector<std::byte>(preparedCold.size());

    preparedHot.writeTo(hotBytes);
    preparedCold.writeTo(coldBytes);

    REQUIRE(validateSerializedHotTrack(hotBytes));
    REQUIRE(validateSerializedColdTrack(coldBytes));
  }

  TEST_CASE("updatePreparedTrackRecord replaces existing hot and cold track records", "[library][unit][track]")
  {
    auto fixture = TrackStoreFixture{};
    auto transaction = writeTransaction(fixture.library);
    auto originalBuilder = TrackBuilder::makeEmpty();
    originalBuilder.metadata().title("Original Track");
    originalBuilder.property().uri("original.flac");
    auto const [originalHot, originalCold] = prepareTrack(originalBuilder, transaction, fixture.library.resources());

    auto updatedBuilder = TrackBuilder::makeEmpty();
    updatedBuilder.metadata().title("Updated Track");
    updatedBuilder.property().uri("updated.flac");
    auto const [updatedHot, updatedCold] = prepareTrack(updatedBuilder, transaction, fixture.library.resources());

    auto writer = fixture.store.writer(transaction);
    auto createResult = createPreparedTrackRecord(writer, originalHot, originalCold);
    REQUIRE(createResult);

    auto const trackId = *createResult;
    auto updateResult = updatePreparedTrackRecord(writer, trackId, updatedHot, updatedCold);
    REQUIRE(updateResult);

    auto optView = writer.get(trackId, TrackStore::Reader::LoadMode::Both);
    REQUIRE(optView);
    CHECK(optView->metadata().title() == "Updated Track");
    CHECK(optView->property().uri() == "updated.flac");
    REQUIRE(transaction.commit());
  }

  TEST_CASE("prepared track data is a snapshot unaffected by later builder mutation", "[library][unit][track]")
  {
    auto fixture = TrackStoreFixture{};
    auto transaction = writeTransaction(fixture.library);
    auto builder = TrackBuilder::makeEmpty();

    {
      // Inputs the builder only borrows as string_views; they go out of
      // scope after prepare to prove the prepared value owns its bytes.
      auto const title = std::string{"Snapshot Title"};
      auto const uri = std::string{"snapshot.flac"};
      builder.metadata().title(title).trackNumber(3);
      builder.property().uri(uri);

      auto const [preparedHot, preparedCold] = prepareTrack(builder, transaction, fixture.library.resources());

      auto const longerTitle = std::string{"Mutated Title That Is Much Longer Than Before"};
      auto const longerUri = std::string{"mutated/path/that/is/much/longer.flac"};
      builder.metadata().title(longerTitle).trackNumber(9);
      builder.property().uri(longerUri);

      auto writer = fixture.store.writer(transaction);

      auto createResult = createPreparedTrackRecord(writer, preparedHot, preparedCold);
      REQUIRE(createResult);

      auto const trackId = *createResult;
      auto const optView = writer.get(trackId, TrackStore::Reader::LoadMode::Both);
      REQUIRE(optView);
      CHECK(trackId != kInvalidTrackId);
      CHECK(optView->isHotValid());
      CHECK(optView->isColdValid());
      CHECK(optView->metadata().title() == "Snapshot Title");
      CHECK(optView->metadata().trackNumber() == 3);
      CHECK(optView->property().uri() == "snapshot.flac");
      REQUIRE(transaction.commit());
    }
  }

  TEST_CASE("MusicLibrary - open rejects a cold-only Track record", "[library][regression][track-store]")
  {
    auto const temp = ao::test::TempDir{};
    seedColdOnlyTrack(temp.path());
    auto const library = openTestMusicLibrary(temp.path(), temp.path());

    REQUIRE_FALSE(library);
    CHECK(library.error().code == Error::Code::CorruptData);
  }

  TEST_CASE("updatePreparedTrackRecord rolls back its hot update when the cold reservation fails",
            "[library][regression][track-store]")
  {
    constexpr std::size_t kMapSize = std::size_t{64} * 1024;
    auto const temp = ao::test::TempDir{};
    auto library =
      ao::test::requireValue(MusicLibrary::open(temp.path(), temp.path(), MusicLibrary::Options{.mapSize = kMapSize}));
    auto originalTransaction = writeTransaction(library);
    auto originalBuilder = TrackBuilder::makeEmpty();
    originalBuilder.metadata().title("Original");
    originalBuilder.property().uri("original.flac");
    auto const [originalHot, originalCold] = prepareTrack(originalBuilder, originalTransaction, library.resources());
    auto originalWriter = library.tracks().writer(originalTransaction);
    auto const createResult = createPreparedTrackRecord(originalWriter, originalHot, originalCold);
    REQUIRE(createResult);
    auto const trackId = *createResult;
    REQUIRE(originalTransaction.commit());

    auto optFailure = std::optional<Error>{};

    {
      auto updateTransaction = writeTransaction(library);
      auto updatedBuilder = TrackBuilder::makeEmpty();
      updatedBuilder.metadata().title("Must roll back");
      // This canonical cold record is larger than the complete LMDB map, while
      // the preceding hot replacement fits in one ordinary page.
      auto const oversizedValue = std::string(std::size_t{60} * 1024, 'x');
      updatedBuilder.property().uri("oversized.flac");
      updatedBuilder.customMetadata().add("oversized", oversizedValue);
      auto const [updatedHot, updatedCold] = prepareTrack(updatedBuilder, updateTransaction, library.resources());
      auto updateWriter = library.tracks().writer(updateTransaction);

      try
      {
        [[maybe_unused]] auto result = updatePreparedTrackRecord(updateWriter, trackId, updatedHot, updatedCold);
        FAIL("updatePreparedTrackRecord should abort when the cold reservation cannot fit");
      }
      catch (lmdb::detail::TransactionFailure const& transactionFailure)
      {
        optFailure = transactionFailure.error();
      }
    }

    REQUIRE(optFailure);
    CHECK(optFailure->code == Error::Code::IoError);

    auto readTransaction = library.readTransaction();
    auto const optView = library.tracks().reader(readTransaction).get(trackId);
    REQUIRE(optView);
    CHECK(optView->metadata().title() == "Original");
    CHECK(optView->property().uri() == "original.flac");
  }
} // namespace ao::library::test
