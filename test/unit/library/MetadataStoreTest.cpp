// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "test/unit/TestUtils.h"
#include "test/unit/library/WritableLibraryTestSupport.h"
#include "test/unit/lmdb/LmdbTestSupport.h"
#include <ao/Error.h>
#include <ao/library/MetadataLayout.h>
#include <ao/library/MetadataStore.h>
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

  TEST_CASE("MetadataStore - load returns CorruptData for invalid metadata header size",
            "[library][unit][metadata-store]")
  {
    auto temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});
    auto wtxn = beginWriteTransaction(env);
    auto db = openDatabase(wtxn, "meta");
    // Seed an invalid physical record; public reads still enter through MusicLibrary.
    auto writer = db.writer(wtxn);
    auto invalidData = std::vector{std::byte{0x42}};
    REQUIRE(writer.create(kMetadataHeaderRecordId, std::span<std::byte const>{invalidData}));
    REQUIRE(wtxn.commit());

    auto const result = MusicLibrary::open(temp.path(), temp.path());
    REQUIRE_FALSE(result);
    CHECK(result.error().code == Error::Code::CorruptData);
  }

  TEST_CASE("MetadataStore - loads the initialized header through a library snapshot",
            "[library][unit][metadata-store]")
  {
    auto const temp = ao::test::TempDir{};
    auto library = MusicLibrary{temp.path(), temp.path() / "db"};
    auto transaction = library.readTransaction();
    auto const loadedResult = library.metadata().load(transaction);

    REQUIRE(loadedResult);
    CHECK(loadedResult->magic == kMetadataMagic);
    CHECK(loadedResult->libraryVersion == kLibraryVersion);
    STATIC_REQUIRE_FALSE(std::is_same_v<ReadTransaction, lmdb::ReadTransaction>);
    STATIC_REQUIRE(std::is_move_constructible_v<ReadTransaction>);
    STATIC_REQUIRE_FALSE(std::is_copy_constructible_v<ReadTransaction>);
  }

  TEST_CASE("MetadataStore - update overwrites previous header values", "[library][unit][metadata-store]")
  {
    auto const temp = ao::test::TempDir{};
    auto library = MusicLibrary{temp.path(), temp.path() / "db"};
    auto header = library.metadataHeader();
    auto transaction = writeTransaction(library);
    header.flags = 42;

    REQUIRE(library.metadata().update(transaction, header));
    auto const stagedResult = library.metadata().load(transaction);
    REQUIRE(stagedResult);
    CHECK(stagedResult->flags == 42);
    REQUIRE(transaction.commit());

    auto readTransaction = library.readTransaction();
    auto const loadedResult = library.metadata().load(readTransaction);
    REQUIRE(loadedResult);
    CHECK(loadedResult->flags == 42);
  }
} // namespace ao::library::test
