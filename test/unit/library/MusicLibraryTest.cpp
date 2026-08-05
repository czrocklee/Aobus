// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/library/MusicLibrary.h>

#include "test/unit/TestFixtureSupport.h"
#include "test/unit/library/MusicLibraryTestSupport.h"
#include "test/unit/library/TrackStoreTestSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/library/WritableLibraryTestSupport.h"
#include "test/unit/lmdb/LmdbTestSupport.h"
#include <ao/Error.h>
#include <ao/Exception.h>
#include <ao/library/FileManifestBuilder.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/ListBuilder.h>
#include <ao/library/ListStore.h>
#include <ao/library/MetadataLayout.h>
#include <ao/library/MetadataStore.h>
#include <ao/library/ResourceStore.h>
#include <ao/library/TrackLayout.h>
#include <ao/library/TrackStore.h>
#include <ao/library/WritableMusicLibrary.h>
#include <ao/lmdb/Environment.h>
#include <ao/lmdb/Transaction.h>
#include <ao/utility/ByteView.h>

#include <catch2/catch_test_macros.hpp>
#include <lmdb.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace ao::library::test
{
  using namespace ao::lmdb;
  using namespace ao::lmdb::test;

  namespace
  {
    void createLibraryMetadataHeader(std::filesystem::path const& path, std::uint32_t libraryVersion)
    {
      auto env = lmdb::test::openEnvironment(path, {.flags = MDB_NOTLS, .maxDatabases = 8});
      auto transaction = lmdb::test::beginWriteTransaction(env);
      auto metadataDatabase = lmdb::test::openDatabase(transaction, "meta");
      auto header = MetadataHeader{.magic = kMetadataMagic,
                                   .libraryVersion = libraryVersion,
                                   .flags = 0,
                                   .createdTime = std::chrono::sys_time{std::chrono::milliseconds{1}}};
      REQUIRE(metadataDatabase.writer(transaction).create(kMetadataHeaderRecordId, utility::bytes::view(header)));
      REQUIRE(transaction.commit());
    }

    void initializeLibrary(std::filesystem::path const& path)
    {
      auto const libraryRes = openTestMusicLibrary(path, path);
      REQUIRE(libraryRes);
    }

    void createRawIntegerRow(std::filesystem::path const& path,
                             std::string const& databaseName,
                             std::uint32_t const id,
                             std::span<std::byte const> const payload)
    {
      auto environment = openEnvironment(path, {.flags = MDB_NOTLS, .maxDatabases = 8});
      auto transaction = beginWriteTransaction(environment);
      auto database = openDatabase(transaction, databaseName);
      REQUIRE(database.writer(transaction).create(id, payload));
      REQUIRE(transaction.commit());
    }

    void createRawIntegerKeyRow(std::filesystem::path const& path,
                                std::string const& databaseName,
                                std::span<std::byte const> const key,
                                std::span<std::byte const> const payload)
    {
      auto environment = openEnvironment(path, {.flags = MDB_NOTLS, .maxDatabases = 8});
      auto transaction = beginWriteTransaction(environment);
      auto database = openDatabase(transaction, databaseName);
      REQUIRE(database.writer(transaction).create(key, payload));
      REQUIRE(transaction.commit());
    }

    void createRawBlobRow(std::filesystem::path const& path,
                          std::string const& databaseName,
                          std::span<std::byte const> const key,
                          std::span<std::byte const> const payload)
    {
      auto environment = openEnvironment(path, {.flags = MDB_NOTLS, .maxDatabases = 8});
      auto transaction = beginWriteTransaction(environment);
      auto database = openDatabase(transaction, databaseName, Database::KeyKind::Blob);
      REQUIRE(database.writer(transaction).create(key, payload));
      REQUIRE(transaction.commit());
    }

    void requireCorruptLibrary(std::filesystem::path const& path)
    {
      auto const result = openTestMusicLibrary(path, path);
      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::CorruptData);
    }
  } // namespace

  TEST_CASE("MusicLibrary - initializes metadata header", "[library][unit][music-library]")
  {
    auto const temp = ao::test::TempDir{};

    auto const firstHeader = [&]
    {
      auto firstRes = openTestMusicLibrary(temp.path(), temp.path());
      REQUIRE(firstRes);
      auto const header = MetadataHeader{firstRes->metadataHeader()};
      CHECK(header.magic == kMetadataMagic);
      CHECK(header.libraryVersion == kLibraryVersion);
      return header;
    }();

    auto reopenedRes = openTestMusicLibrary(temp.path(), temp.path());
    REQUIRE(reopenedRes);
    auto const& reopened = *reopenedRes;
    CHECK(reopened.metadataHeader().libraryId == firstHeader.libraryId);
    CHECK(reopened.metadataHeader().createdTime == firstHeader.createdTime);
  }

  TEST_CASE("MusicLibrary - rejects persisted data without metadata", "[library][unit][music-library][integrity]")
  {
    auto const temp = ao::test::TempDir{};
    createRawIntegerRow(temp.path(), "dictionary", 1, createStringData("orphaned"));

    requireCorruptLibrary(temp.path());
  }

  TEST_CASE("MusicLibrary - reports unsupported library versions as CorruptData", "[library][unit][music-library]")
  {
    auto const temp = ao::test::TempDir{};
    constexpr std::uint32_t kLegacyV1LibraryVersion = 1;
    constexpr std::uint32_t kPreviousColdLayoutLibraryVersion = 2;
    constexpr std::uint32_t kPreUnifiedListOrderingLibraryVersion = 4;

    SECTION("future version")
    {
      createLibraryMetadataHeader(temp.path(), kLibraryVersion + 1);

      auto const result = openTestMusicLibrary(temp.path(), temp.path());
      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::CorruptData);
    }

    SECTION("old version")
    {
      static_assert(kLegacyV1LibraryVersion != kLibraryVersion);
      createLibraryMetadataHeader(temp.path(), kLegacyV1LibraryVersion);

      auto const result = openTestMusicLibrary(temp.path(), temp.path());
      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::CorruptData);
    }

    SECTION("previous cold layout version")
    {
      static_assert(kPreviousColdLayoutLibraryVersion != kLibraryVersion);
      createLibraryMetadataHeader(temp.path(), kPreviousColdLayoutLibraryVersion);

      auto const result = openTestMusicLibrary(temp.path(), temp.path());
      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::CorruptData);
    }

    SECTION("version 4 before unified List ordering")
    {
      static_assert(kPreUnifiedListOrderingLibraryVersion != kLibraryVersion);
      createLibraryMetadataHeader(temp.path(), kPreUnifiedListOrderingLibraryVersion);

      auto const result = openTestMusicLibrary(temp.path(), temp.path());
      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::CorruptData);
    }
  }

  TEST_CASE("MusicLibrary - open reports a storage fault as a Result", "[library][regression][music-library]")
  {
    // open() is the sole public recoverable constructor. Storage mutations
    // inside it raise TransactionFailure to leave the initialization
    // transaction; that lexical unwind marker must never reach the caller.
    auto const temp = ao::test::TempDir{};
    constexpr std::size_t kUnusableMapSize = std::size_t{8} * 1024;

    auto const result =
      MusicLibrary::open(temp.path(), temp.path() / "tiny-db", MusicLibrary::Options{.mapSize = kUnusableMapSize});

    REQUIRE_FALSE(result);
    CHECK(result.error().code == Error::Code::IoError);
  }

  TEST_CASE("MusicLibrary - rejects invalid persisted dictionary state", "[library][unit][music-library][integrity]")
  {
    auto const temp = ao::test::TempDir{};
    initializeLibrary(temp.path());

    SECTION("dictionary ids must be dense from one")
    {
      createRawIntegerRow(temp.path(), "dictionary", 2, createStringData("gap"));
      requireCorruptLibrary(temp.path());
    }

    SECTION("dictionary keys must have the native 32-bit width")
    {
      auto const shortKey = std::array{std::byte{1}, std::byte{0}};
      createRawIntegerKeyRow(temp.path(), "dictionary", shortKey, createStringData("short"));
      requireCorruptLibrary(temp.path());
    }

    SECTION("dictionary text must be unique")
    {
      createRawIntegerRow(temp.path(), "dictionary", 1, createStringData("duplicate"));
      createRawIntegerRow(temp.path(), "dictionary", 2, createStringData("duplicate"));
      requireCorruptLibrary(temp.path());
    }
  }

  TEST_CASE("MusicLibrary - rejects invalid persisted Track state", "[library][unit][music-library][integrity]")
  {
    auto const temp = ao::test::TempDir{};
    initializeLibrary(temp.path());

    SECTION("Track keys must be nonzero")
    {
      createRawIntegerRow(temp.path(), "tracks_hot", 0, makeHotData());
      createRawIntegerRow(temp.path(), "tracks_cold", 0, makeColdData());
      requireCorruptLibrary(temp.path());
    }

    SECTION("Track payloads must be canonical")
    {
      auto const invalidHot = std::array{std::byte{0x42}};
      createRawIntegerRow(temp.path(), "tracks_hot", 1, invalidHot);
      createRawIntegerRow(temp.path(), "tracks_cold", 1, makeColdData());
      requireCorruptLibrary(temp.path());
    }

    SECTION("Track dictionary references must resolve")
    {
      createRawIntegerRow(temp.path(), "tracks_hot", 1, makeHotData(TrackHotHeader{.artistId = DictionaryId{1}}));
      createRawIntegerRow(temp.path(), "tracks_cold", 1, makeColdData());
      requireCorruptLibrary(temp.path());
    }
  }

  TEST_CASE("MusicLibrary - rejects invalid persisted List state", "[library][unit][music-library][integrity]")
  {
    auto const temp = ao::test::TempDir{};
    initializeLibrary(temp.path());

    SECTION("List keys must be nonzero")
    {
      auto const validPayload = ao::test::requireValue(ListBuilder::makeEmpty().serialize());
      createRawIntegerRow(temp.path(), "lists", 0, validPayload);
      requireCorruptLibrary(temp.path());
    }

    SECTION("List keys must have the native 32-bit width")
    {
      auto const shortKey = std::array{std::byte{1}, std::byte{0}};
      auto const validPayload = ao::test::requireValue(ListBuilder::makeEmpty().serialize());
      createRawIntegerKeyRow(temp.path(), "lists", shortKey, validPayload);
      requireCorruptLibrary(temp.path());
    }

    SECTION("List payloads must have the exact canonical layout")
    {
      auto const invalidPayload = std::array{std::byte{0x42}};
      createRawIntegerRow(temp.path(), "lists", 1, invalidPayload);
      requireCorruptLibrary(temp.path());
    }
  }

  TEST_CASE("MusicLibrary - rejects invalid persisted manifest state", "[library][unit][music-library][integrity]")
  {
    auto const temp = ao::test::TempDir{};
    initializeLibrary(temp.path());

    SECTION("manifest keys must be canonical library URIs")
    {
      auto const invalidKey = utility::bytes::view(std::string_view{"../x"});
      auto const validPayload = FileManifestBuilder::makeEmpty().trackId(TrackId{1}).serialize();
      createRawBlobRow(temp.path(), "file_manifest", invalidKey, validPayload);
      requireCorruptLibrary(temp.path());
    }

    SECTION("manifest payloads must have the exact canonical layout")
    {
      auto const validKey = utility::bytes::view(std::string_view{"song"});
      auto const invalidPayload = std::array{std::byte{0x42}};
      createRawBlobRow(temp.path(), "file_manifest", validKey, invalidPayload);
      requireCorruptLibrary(temp.path());
    }
  }

  TEST_CASE("MusicLibrary - accessors return valid references", "[library][unit][music-library]")
  {
    auto const temp = ao::test::TempDir{};
    auto const ml = makeTestMusicLibrary(temp.path(), temp.path());

    // All store accessors should be callable without crashing
    CHECK_NOTHROW(ml.tracks());
    CHECK_NOTHROW(ml.lists());
    CHECK_NOTHROW(ml.resources());
    CHECK_NOTHROW(ml.dictionary());
    CHECK_NOTHROW(ml.manifest());
    CHECK(ml.rootPath() == temp.path());

    STATIC_REQUIRE(std::is_same_v<decltype(std::declval<MusicLibrary&>().tracks()), TrackStore const&>);
    STATIC_REQUIRE(std::is_same_v<decltype(std::declval<MusicLibrary&>().lists()), ListStore const&>);
    STATIC_REQUIRE(std::is_same_v<decltype(std::declval<MusicLibrary&>().resources()), ResourceStore const&>);
    STATIC_REQUIRE(std::is_same_v<decltype(std::declval<MusicLibrary&>().dictionary()), DictionaryStore const&>);
    STATIC_REQUIRE(std::is_same_v<decltype(std::declval<MusicLibrary&>().manifest()), FileManifestStore const&>);
    STATIC_REQUIRE(std::is_same_v<decltype(std::declval<MusicLibrary&>().metadata()), MetadataStore const&>);
  }

  TEST_CASE("MusicLibrary - read and write transactions work", "[library][unit][music-library]")
  {
    auto const temp = ao::test::TempDir{};
    auto ml = makeTestMusicLibrary(temp.path(), temp.path());

    auto wtxn = writeTransaction(ml);
    CHECK_NOTHROW(wtxn.commit());

    auto rtxn = ml.readTransaction(); // validates read access to the database
  }

  TEST_CASE("WritableMusicLibrary - excludes another writer session until release",
            "[library][unit][music-library][concurrency]")
  {
    auto const temp = ao::test::TempDir{};
    auto firstLibrary = makeTestMusicLibrary(temp.path(), temp.path() / "db");
    auto secondLibrary = makeTestMusicLibrary(temp.path(), temp.path() / "db");

    {
      auto firstWriterRes = WritableMusicLibrary::acquire(firstLibrary);
      REQUIRE(firstWriterRes);

      auto secondWriterRes = WritableMusicLibrary::acquire(secondLibrary);
      REQUIRE_FALSE(secondWriterRes);
      CHECK(secondWriterRes.error().code == Error::Code::Conflict);

      auto transaction = firstWriterRes->writeTransaction();
      REQUIRE(transaction.commit());
    }

    auto releasedWriterRes = WritableMusicLibrary::acquire(secondLibrary);
    REQUIRE(releasedWriterRes);
  }

  TEST_CASE("WritableMusicLibrary - active transaction retains the writer session",
            "[library][unit][music-library][concurrency]")
  {
    auto const temp = ao::test::TempDir{};
    auto firstLibrary = makeTestMusicLibrary(temp.path(), temp.path() / "db");
    auto secondLibrary = makeTestMusicLibrary(temp.path(), temp.path() / "db");
    auto optTransaction = std::optional<WriteTransaction>{};

    {
      auto writerRes = WritableMusicLibrary::acquire(firstLibrary);
      REQUIRE(writerRes);
      optTransaction.emplace(writerRes->writeTransaction());
    }

    auto activeTransactionWriterRes = WritableMusicLibrary::acquire(secondLibrary);
    REQUIRE_FALSE(activeTransactionWriterRes);
    CHECK(activeTransactionWriterRes.error().code == Error::Code::Conflict);

    REQUIRE(optTransaction->commit());
    auto committedTransactionWriterRes = WritableMusicLibrary::acquire(secondLibrary);
    REQUIRE(committedTransactionWriterRes);
  }

  TEST_CASE("WritableMusicLibrary - terminal transaction paths release the retained writer session",
            "[library][unit][music-library][concurrency]")
  {
    auto const temp = ao::test::TempDir{};
    auto firstLibrary = makeTestMusicLibrary(temp.path(), temp.path() / "db");
    auto secondLibrary = makeTestMusicLibrary(temp.path(), temp.path() / "db");

    SECTION("abort by destruction")
    {
      {
        auto writerRes = WritableMusicLibrary::acquire(firstLibrary);
        REQUIRE(writerRes);
        auto transaction = writerRes->writeTransaction();
      }

      REQUIRE(WritableMusicLibrary::acquire(secondLibrary));
    }

    SECTION("explicit abort")
    {
      auto optTransaction = std::optional<WriteTransaction>{};

      {
        auto writerRes = WritableMusicLibrary::acquire(firstLibrary);
        REQUIRE(writerRes);
        optTransaction.emplace(writerRes->writeTransaction());
      }

      optTransaction->abort();
      REQUIRE(WritableMusicLibrary::acquire(secondLibrary));
      auto commitRes = optTransaction->commit();
      REQUIRE_FALSE(commitRes);
      CHECK(commitRes.error().code == Error::Code::InvalidState);
    }

    SECTION("commit failure")
    {
      auto optTransaction = std::optional<WriteTransaction>{};

      {
        auto writerRes = WritableMusicLibrary::acquire(firstLibrary);
        REQUIRE(writerRes);
        optTransaction.emplace(writerRes->writeTransaction(WriteTransaction::Options{
          .optInjectedCommitFailure = Error{.code = Error::Code::IoError, .message = "injected failure"},
        }));
      }

      auto commitRes = optTransaction->commit();
      REQUIRE_FALSE(commitRes);
      CHECK(commitRes.error().code == Error::Code::IoError);
      REQUIRE(WritableMusicLibrary::acquire(secondLibrary));
    }

    SECTION("storage mutation failure unwinds and rolls back")
    {
      constexpr std::size_t kMapSize = std::size_t{256} * 1024;
      auto smallLibrary = ao::test::requireValue(
        MusicLibrary::open(temp.path(), temp.path() / "small-db", MusicLibrary::Options{.mapSize = kMapSize}));
      auto secondSmallLibrary = ao::test::requireValue(
        MusicLibrary::open(temp.path(), temp.path() / "small-db", MusicLibrary::Options{.mapSize = kMapSize}));
      {
        auto writerRes = WritableMusicLibrary::acquire(smallLibrary);
        REQUIRE(writerRes);
        auto transaction = writerRes->writeTransaction();
        auto const oversizedValue = std::vector<std::byte>(kMapSize * 4);
        auto failureRes =
          transaction.apply([&smallLibrary, &oversizedValue](WriteTransaction& activeTransaction)
                            { return smallLibrary.resources().writer(activeTransaction).create(oversizedValue); });
        REQUIRE_FALSE(failureRes);
        CHECK(failureRes.error().code == Error::Code::IoError);
      }

      REQUIRE(WritableMusicLibrary::acquire(secondSmallLibrary));
    }
  }

  TEST_CASE("MusicLibrary - moved-from write transactions are inactive", "[library][unit][music-library]")
  {
    auto const temp = ao::test::TempDir{};
    auto library = makeTestMusicLibrary(temp.path(), temp.path());
    auto source = writeTransaction(library);
    auto destination = std::move(source);

    // The wrapper specifies an inactive moved-from state that is safe to query.
    // NOLINTNEXTLINE(bugprone-use-after-move)
    CHECK_THROWS_AS(source.dictionary(), Exception);
    auto const sourceCommitRes = source.commit();
    REQUIRE_FALSE(sourceCommitRes);
    CHECK(sourceCommitRes.error().code == Error::Code::InvalidState);
    REQUIRE(destination.commit());
  }

  TEST_CASE("MusicLibrary - write transaction commit is terminal", "[library][unit][music-library]")
  {
    auto const temp = ao::test::TempDir{};
    auto library = makeTestMusicLibrary(temp.path(), temp.path());
    auto transaction = writeTransaction(library);

    REQUIRE(transaction.commit());
    auto const repeatedCommitRes = transaction.commit();
    REQUIRE_FALSE(repeatedCommitRes);
    CHECK(repeatedCommitRes.error().code == Error::Code::InvalidState);
    CHECK_THROWS_AS(transaction.dictionary(), Exception);
  }

  TEST_CASE("MusicLibrary - moved-from read transactions are inactive", "[library][unit][music-library]")
  {
    auto const temp = ao::test::TempDir{};
    auto library = makeTestMusicLibrary(temp.path(), temp.path());
    auto source = library.readTransaction();
    auto destination = std::move(source);

    // The wrapper specifies an inactive moved-from state that is safe to query.
    // NOLINTNEXTLINE(bugprone-use-after-move)
    CHECK_THROWS_AS(library.tracks().reader(source), Exception);
    CHECK_NOTHROW(library.tracks().reader(destination));
  }

  TEST_CASE("MusicLibrary - rejects transactions from another library", "[library][unit][music-library]")
  {
    auto const temp = ao::test::TempDir{};
    auto libraryA = makeTestMusicLibrary(temp.path() / "music-a", temp.path() / "db-a");
    auto libraryB = makeTestMusicLibrary(temp.path() / "music-b", temp.path() / "db-b");
    auto const libraryBHeader = libraryB.metadataHeader();

    {
      auto const transaction = libraryA.readTransaction();
      CHECK_THROWS_AS(libraryB.tracks().reader(transaction), Exception);
      CHECK_THROWS_AS(libraryB.lists().reader(transaction), Exception);
      CHECK_THROWS_AS(libraryB.resources().reader(transaction), Exception);
      CHECK_THROWS_AS(libraryB.manifest().reader(transaction), Exception);
      CHECK_THROWS_AS(libraryB.metadata().load(transaction), Exception);
      CHECK_THROWS_AS(libraryB.libraryRevision(transaction), Exception);
    }

    {
      auto transaction = writeTransaction(libraryA);
      CHECK_THROWS_AS(libraryB.tracks().reader(transaction), Exception);
      CHECK_THROWS_AS(libraryB.tracks().writer(transaction), Exception);
      CHECK_THROWS_AS(libraryB.lists().reader(transaction), Exception);
      CHECK_THROWS_AS(libraryB.lists().writer(transaction), Exception);
      CHECK_THROWS_AS(libraryB.resources().reader(transaction), Exception);
      CHECK_THROWS_AS(libraryB.resources().writer(transaction), Exception);
      CHECK_THROWS_AS(libraryB.manifest().reader(transaction), Exception);
      CHECK_THROWS_AS(libraryB.manifest().writer(transaction), Exception);
      CHECK_THROWS_AS(libraryB.metadata().load(transaction), Exception);
      CHECK_THROWS_AS(libraryB.metadata().update(transaction, libraryBHeader), Exception);
      CHECK_THROWS_AS(libraryB.libraryRevision(transaction), Exception);
    }
  }
} // namespace ao::library::test
