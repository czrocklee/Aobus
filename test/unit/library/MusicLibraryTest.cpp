// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/TestUtils.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/library/WritableLibraryTestSupport.h"
#include "test/unit/lmdb/LmdbTestSupport.h"
#include <ao/Error.h>
#include <ao/Exception.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/ListStore.h>
#include <ao/library/MetadataLayout.h>
#include <ao/library/MetadataStore.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/ResourceStore.h>
#include <ao/library/TrackStore.h>
#include <ao/library/WritableMusicLibrary.h>
#include <ao/lmdb/Environment.h>
#include <ao/lmdb/Transaction.h>
#include <ao/utility/ByteView.h>

#include <catch2/catch_test_macros.hpp>
#include <lmdb.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <type_traits>
#include <utility>

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
  } // namespace

  TEST_CASE("MusicLibrary - initializes metadata header", "[library][unit][music-library]")
  {
    auto const temp = ao::test::TempDir{};

    auto const firstHeader = [&]
    {
      auto firstResult = openTestMusicLibrary(temp.path(), temp.path());
      REQUIRE(firstResult);
      auto const header = MetadataHeader{firstResult->metadataHeader()};
      CHECK(header.magic == kMetadataMagic);
      CHECK(header.libraryVersion == kLibraryVersion);
      return header;
    }();

    auto reopenedResult = openTestMusicLibrary(temp.path(), temp.path());
    REQUIRE(reopenedResult);
    auto const& reopened = *reopenedResult;
    CHECK(reopened.metadataHeader().libraryId == firstHeader.libraryId);
    CHECK(reopened.metadataHeader().createdTime == firstHeader.createdTime);
  }

  TEST_CASE("MusicLibrary - reports unsupported library versions as CorruptData", "[library][unit][music-library]")
  {
    auto const temp = ao::test::TempDir{};
    constexpr std::uint32_t kLegacyV1LibraryVersion = 1;
    constexpr std::uint32_t kPreviousColdLayoutLibraryVersion = 2;

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
      auto firstWriterResult = WritableMusicLibrary::acquire(firstLibrary);
      REQUIRE(firstWriterResult);

      auto secondWriterResult = WritableMusicLibrary::acquire(secondLibrary);
      REQUIRE_FALSE(secondWriterResult);
      CHECK(secondWriterResult.error().code == Error::Code::Conflict);

      auto transaction = firstWriterResult->writeTransaction();
      REQUIRE(transaction.commit());
    }

    auto releasedWriterResult = WritableMusicLibrary::acquire(secondLibrary);
    REQUIRE(releasedWriterResult);
  }

  TEST_CASE("WritableMusicLibrary - active transaction retains the writer session",
            "[library][unit][music-library][concurrency]")
  {
    auto const temp = ao::test::TempDir{};
    auto firstLibrary = makeTestMusicLibrary(temp.path(), temp.path() / "db");
    auto secondLibrary = makeTestMusicLibrary(temp.path(), temp.path() / "db");
    auto optTransaction = std::optional<WriteTransaction>{};

    {
      auto writerResult = WritableMusicLibrary::acquire(firstLibrary);
      REQUIRE(writerResult);
      optTransaction.emplace(writerResult->writeTransaction());
    }

    auto activeTransactionWriterResult = WritableMusicLibrary::acquire(secondLibrary);
    REQUIRE_FALSE(activeTransactionWriterResult);
    CHECK(activeTransactionWriterResult.error().code == Error::Code::Conflict);

    REQUIRE(optTransaction->commit());
    auto committedTransactionWriterResult = WritableMusicLibrary::acquire(secondLibrary);
    REQUIRE(committedTransactionWriterResult);
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
        auto writerResult = WritableMusicLibrary::acquire(firstLibrary);
        REQUIRE(writerResult);
        auto transaction = writerResult->writeTransaction();
      }

      REQUIRE(WritableMusicLibrary::acquire(secondLibrary));
    }

    SECTION("explicit abort")
    {
      auto optTransaction = std::optional<WriteTransaction>{};

      {
        auto writerResult = WritableMusicLibrary::acquire(firstLibrary);
        REQUIRE(writerResult);
        optTransaction.emplace(writerResult->writeTransaction());
      }

      optTransaction->abort();
      REQUIRE(WritableMusicLibrary::acquire(secondLibrary));
      auto commitResult = optTransaction->commit();
      REQUIRE_FALSE(commitResult);
      CHECK(commitResult.error().code == Error::Code::InvalidState);
    }

    SECTION("commit failure")
    {
      auto optTransaction = std::optional<WriteTransaction>{};

      {
        auto writerResult = WritableMusicLibrary::acquire(firstLibrary);
        REQUIRE(writerResult);
        optTransaction.emplace(writerResult->writeTransaction(WriteTransaction::Options{
          .optInjectedCommitFailure = Error{.code = Error::Code::IoError, .message = "injected failure"},
        }));
      }

      auto commitResult = optTransaction->commit();
      REQUIRE_FALSE(commitResult);
      CHECK(commitResult.error().code == Error::Code::IoError);
      REQUIRE(WritableMusicLibrary::acquire(secondLibrary));
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
    auto const sourceCommit = source.commit();
    REQUIRE_FALSE(sourceCommit);
    CHECK(sourceCommit.error().code == Error::Code::InvalidState);
    REQUIRE(destination.commit());
  }

  TEST_CASE("MusicLibrary - write transaction commit is terminal", "[library][unit][music-library]")
  {
    auto const temp = ao::test::TempDir{};
    auto library = makeTestMusicLibrary(temp.path(), temp.path());
    auto transaction = writeTransaction(library);

    REQUIRE(transaction.commit());
    auto const repeatedCommit = transaction.commit();
    REQUIRE_FALSE(repeatedCommit);
    CHECK(repeatedCommit.error().code == Error::Code::InvalidState);
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
    auto libraryA = MusicLibrary{temp.path() / "music-a", temp.path() / "db-a"};
    auto libraryB = MusicLibrary{temp.path() / "music-b", temp.path() / "db-b"};
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
