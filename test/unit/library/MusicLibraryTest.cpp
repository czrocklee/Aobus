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

TEST_CASE("MusicLibrary initializes metadata header", "[core][library]")
{
  auto temp = TempDir{};

  auto const first = ao::library::MusicLibrary{temp.path()};
  auto const firstHeader = first.metaHeader();

  REQUIRE(firstHeader.magic == ao::library::kLibraryMetaMagic);
  REQUIRE(firstHeader.libraryVersion == ao::library::kLibraryVersion);

  auto const reopened = ao::library::MusicLibrary{temp.path()};
  REQUIRE(reopened.metaHeader().libraryId == firstHeader.libraryId);
  REQUIRE(reopened.metaHeader().createdAtUnixMs == firstHeader.createdAtUnixMs);
}

TEST_CASE("MusicLibrary rejects unsupported library versions", "[core][library]")
{
  auto temp = TempDir{};
  auto env = ao::lmdb::Environment{temp.path(), {.flags = MDB_NOTLS, .maxDatabases = 8}};

  auto txn = ao::lmdb::WriteTransaction{env};
  auto metaStore = ao::library::MetaStore{ao::lmdb::Database{txn, "meta"}};
  auto header = ao::library::MetaHeader{.magic = ao::library::kLibraryMetaMagic,
                                        .libraryVersion = ao::library::kLibraryVersion + 1,
                                        .flags = 0,
                                        .createdAtUnixMs = 1,
                                        .migratedAtUnixMs = 1,
                                        .libraryId = {}};
  metaStore.create(txn, header);
  txn.commit();

  REQUIRE_THROWS_AS(ao::library::MusicLibrary{temp.path()}, ao::Exception);
}
