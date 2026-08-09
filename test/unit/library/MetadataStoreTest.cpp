// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "test/unit/TestFixtureSupport.h"
#include "test/unit/library/MusicLibraryTestSupport.h"
#include "test/unit/library/WritableLibraryTestSupport.h"
#include "test/unit/lmdb/LmdbTestSupport.h"
#include <ao/Error.h>
#include <ao/library/LibraryWrite.h>
#include <ao/library/MetadataLayout.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/ReadTransaction.h>
#include <ao/lmdb/Database.h>
#include <ao/lmdb/Environment.h>
#include <ao/lmdb/Transaction.h>

#include <catch2/catch_test_macros.hpp>
#include <lmdb.h>

#include <cstddef>
#include <span>
#include <type_traits>
#include <vector>

namespace ao::library::test
{
  using namespace ao::lmdb;
  using namespace ao::lmdb::test;

  TEST_CASE("MusicLibrary metadata - open returns CorruptData for an invalid header size",
            "[library][unit][music-library][integrity]")
  {
    auto temp = ao::test::TempDir{};

    {
      auto env = openEnvironment(temp.path(), {.flags = MDB_NOTLS, .maxDatabases = 20});
      auto wtxn = beginWriteTransaction(env);
      auto db = openDatabase(wtxn, "meta");
      // Seed an invalid physical record; public reads still enter through MusicLibrary.
      auto writer = db.writer(wtxn);
      auto invalidData = std::vector{std::byte{0x42}};
      REQUIRE(writer.create(kMetadataHeaderRecordId, std::span<std::byte const>{invalidData}));
      REQUIRE(wtxn.commit());
    }

    auto const result = MusicLibrary::open(temp.path(), temp.path());
    REQUIRE_FALSE(result);
    CHECK(result.error().code == Error::Code::CorruptData);
  }

  TEST_CASE("MusicLibrary metadata - snapshot exposes the admitted header", "[library][unit][music-library]")
  {
    auto const temp = ao::test::TempDir{};
    auto library = makeTestMusicLibrary(temp.path(), temp.path() / "db");
    auto transaction = library.readTransaction();
    auto const header = library.metadataHeader(transaction);

    CHECK(header.magic == kMetadataMagic);
    CHECK(header.libraryVersion == kLibraryVersion);
    STATIC_REQUIRE_FALSE(std::is_same_v<ReadTransaction, lmdb::ReadTransaction>);
    STATIC_REQUIRE(std::is_move_constructible_v<ReadTransaction>);
    STATIC_REQUIRE_FALSE(std::is_copy_constructible_v<ReadTransaction>);
  }

  TEST_CASE("MusicLibrary metadata - identity restore publishes only after commit", "[library][unit][music-library]")
  {
    auto const temp = ao::test::TempDir{};
    auto library = makeTestMusicLibrary(temp.path(), temp.path() / "db");
    auto const before = library.metadataHeader();
    auto replacementId = before.libraryId;
    replacementId.front() ^= std::byte{0xff};
    auto transaction = writeTransaction(library);

    REQUIRE(transaction.apply(
      [&library, &replacementId](LibraryWrite& write) -> Result<>
      {
        REQUIRE(write.restoreLibraryIdentity(replacementId));
        CHECK(library.metadataHeader(write).libraryId == replacementId);
        return {};
      }));
    CHECK(library.metadataHeader().libraryId == before.libraryId);
    CHECK(library.metadataHeader(transaction).libraryId == replacementId);
    REQUIRE(transaction.commit());

    auto readTransaction = library.readTransaction();
    CHECK(library.metadataHeader().libraryId == replacementId);
    CHECK(library.metadataHeader(readTransaction).libraryId == replacementId);
  }

  TEST_CASE("MusicLibrary metadata - revision candidate publishes exactly once at commit",
            "[library][unit][write-transaction][revision]")
  {
    auto const temp = ao::test::TempDir{};
    auto library = makeTestMusicLibrary(temp.path(), temp.path() / "db");

    {
      auto read = library.readTransaction();
      CHECK(library.libraryRevision(read) == 0);
    }

    auto firstWrite = writeTransaction(library);
    CHECK(library.libraryRevision(firstWrite) == 1);
    REQUIRE(firstWrite.commit());

    {
      auto read = library.readTransaction();
      CHECK(library.libraryRevision(read) == 1);
    }

    auto abortedWrite = writeTransaction(library);
    CHECK(library.libraryRevision(abortedWrite) == 2);
    abortedWrite.abort();

    auto read = library.readTransaction();
    CHECK(library.libraryRevision(read) == 1);
  }

  TEST_CASE("MusicLibrary metadata - failed commit leaves no durable candidate revision",
            "[library][regression][write-transaction][revision]")
  {
    auto const temp = ao::test::TempDir{};
    auto const databasePath = temp.path() / "db";
    auto library = makeTestMusicLibrary(temp.path(), databasePath);
    auto write = writeTransaction(
      library,
      WriteTransaction::Options{.optInjectedCommitFailure =
                                  Error{.code = Error::Code::IoError, .message = "injected commit failure"}});

    CHECK(library.libraryRevision(write) == 1);
    auto commitRes = write.commit();
    REQUIRE_FALSE(commitRes);
    CHECK(commitRes.error().code == Error::Code::IoError);

    {
      auto read = library.readTransaction();
      CHECK(library.libraryRevision(read) == 0);
    }

    auto reopened = makeTestMusicLibrary(temp.path(), databasePath);
    auto reopenedRead = reopened.readTransaction();
    CHECK(reopened.libraryRevision(reopenedRead) == 0);
  }

  TEST_CASE("MusicLibrary metadata - candidate revision comes from the durable writer snapshot",
            "[library][regression][write-transaction][concurrency]")
  {
    auto const temp = ao::test::TempDir{};
    auto const databasePath = temp.path() / "db";
    auto first = makeTestMusicLibrary(temp.path(), databasePath);
    auto second = makeTestMusicLibrary(temp.path(), databasePath);

    {
      auto write = writeTransaction(first);
      CHECK(first.libraryRevision(write) == 1);
      REQUIRE(write.commit());
    }

    {
      auto write = writeTransaction(second);
      CHECK(second.libraryRevision(write) == 2);
      REQUIRE(write.commit());
    }

    auto firstRead = first.readTransaction();
    auto secondRead = second.readTransaction();
    CHECK(first.libraryRevision(firstRead) == 2);
    CHECK(second.libraryRevision(secondRead) == 2);
  }
} // namespace ao::library::test
