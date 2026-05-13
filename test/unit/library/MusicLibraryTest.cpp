// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators_all.hpp>
#include <catch2/matchers/catch_matchers_all.hpp>

#include <ao/Exception.h>
#include <ao/library/Meta.h>
#include <ao/library/MetaStore.h>
#include <ao/library/MusicLibrary.h>
#include <ao/lmdb/Database.h>
#include <ao/lmdb/Environment.h>
#include <ao/lmdb/Transaction.h>
#include <test/unit/lmdb/TestUtils.h>

namespace ao::library::test
{
  using namespace ao::lmdb::test;

  TEST_CASE("MusicLibrary initializes metadata header", "[core][library]")
  {
    auto const temp = TempDir{};

    auto const first = MusicLibrary{temp.path()};
    auto const firstHeader = first.metaHeader();

    REQUIRE(firstHeader.magic == kLibraryMetaMagic);
    REQUIRE(firstHeader.libraryVersion == kLibraryVersion);

    auto const reopened = MusicLibrary{temp.path()};
    REQUIRE(reopened.metaHeader().libraryId == firstHeader.libraryId);
    REQUIRE(reopened.metaHeader().createdAtUnixMs == firstHeader.createdAtUnixMs);
  }

  TEST_CASE("MusicLibrary rejects unsupported library versions", "[core][library]")
  {
    auto const temp = TempDir{};
    auto env = lmdb::Environment{temp.path(), {.flags = MDB_NOTLS, .maxDatabases = 8}};

    auto txn = lmdb::WriteTransaction{env};
    auto metaStore = MetaStore{lmdb::Database{txn, "meta"}};
    auto header = MetaHeader{.magic = kLibraryMetaMagic,
                             .libraryVersion = kLibraryVersion + 1,
                             .flags = 0,
                             .createdAtUnixMs = 1,
                             .migratedAtUnixMs = 1,
                             .libraryId = {}};
    metaStore.create(txn, header);
    txn.commit();

    REQUIRE_THROWS_AS(MusicLibrary{temp.path()}, Exception);
  }
} // namespace ao::library::test
