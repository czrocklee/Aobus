// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "test/unit/lmdb/TestUtils.h"
#include <ao/Exception.h>
#include <ao/library/Meta.h>
#include <ao/library/MetaStore.h>
#include <ao/library/MusicLibrary.h>
#include <ao/lmdb/Database.h>
#include <ao/lmdb/Environment.h>
#include <ao/lmdb/Transaction.h>

#include <catch2/catch_test_macros.hpp>
#include <lmdb.h>

namespace ao::library::test
{
  using namespace ao::lmdb;
  using namespace ao::lmdb::test;

  TEST_CASE("MusicLibrary initializes metadata header", "[library][unit]")
  {
    auto const temp = TempDir{};

    auto const first = MusicLibrary{temp.path(), temp.path()};
    auto const firstHeader = MetaHeader{first.metaHeader()};

    REQUIRE(firstHeader.magic == kLibraryMetaMagic);
    REQUIRE(firstHeader.libraryVersion == kLibraryVersion);

    auto const reopened = MusicLibrary{temp.path(), temp.path()};
    REQUIRE(reopened.metaHeader().libraryId == firstHeader.libraryId);
    REQUIRE(reopened.metaHeader().createdAtUnixMs == firstHeader.createdAtUnixMs);
  }

  TEST_CASE("MusicLibrary rejects unsupported library versions", "[library][unit]")
  {
    auto const temp = TempDir{};
    auto env = lmdb::Environment{temp.path(), {.flags = MDB_NOTLS, .maxDatabases = 8}};

    auto txn = lmdb::WriteTransaction{env};
    auto metaStore = MetaStore{lmdb::Database{txn, "meta"}};
    auto header = MetaHeader{.magic = kLibraryMetaMagic,
                             .libraryVersion = kLibraryVersion + 1,
                             .flags = 0,
                             .createdAtUnixMs = 1,
                             .libraryId = {}};
    metaStore.create(txn, header);
    txn.commit();

    REQUIRE_THROWS_AS((MusicLibrary{temp.path(), temp.path()}), Exception);
  }

  TEST_CASE("MusicLibrary - accessors return valid references", "[library][unit]")
  {
    auto const temp = TempDir{};
    auto const ml = MusicLibrary{temp.path(), temp.path()};

    // All store accessors should be callable without crashing
    CHECK_NOTHROW(ml.tracks());
    CHECK_NOTHROW(ml.lists());
    CHECK_NOTHROW(ml.resources());
    CHECK_NOTHROW(ml.dictionary());
    CHECK_NOTHROW(ml.manifest());
    CHECK(ml.rootPath() == temp.path());
  }

  TEST_CASE("MusicLibrary - read and write transactions work", "[library][unit]")
  {
    auto const temp = TempDir{};
    auto ml = MusicLibrary{temp.path(), temp.path()};

    auto wtxn = ml.writeTransaction();
    CHECK_NOTHROW(wtxn.commit());

    auto rtxn = ml.readTransaction(); // validates read access to the database
  }
} // namespace ao::library::test
