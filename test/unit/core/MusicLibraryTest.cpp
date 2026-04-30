// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_all.hpp>
#include <catch2/generators/catch_generators_all.hpp>
#include <catch2/catch_approx.hpp>

#include <rs/Exception.h>
#include <rs/core/LibraryMeta.h>
#include <rs/core/LibraryMetaStore.h>
#include <rs/core/MusicLibrary.h>
#include <rs/lmdb/Database.h>
#include <rs/lmdb/Environment.h>
#include <rs/lmdb/Transaction.h>
#include <test/unit/lmdb/TestUtils.h>

TEST_CASE("MusicLibrary initializes metadata header", "[core][library]")
{
  auto temp = TempDir{};

  auto const first = rs::core::MusicLibrary{temp.path()};
  auto const firstHeader = first.metaHeader();

  REQUIRE(firstHeader.magic == rs::core::kLibraryMetaMagic);
  REQUIRE(firstHeader.libraryVersion == rs::core::kLibraryVersion);

  auto const reopened = rs::core::MusicLibrary{temp.path()};
  REQUIRE(reopened.metaHeader().libraryId == firstHeader.libraryId);
  REQUIRE(reopened.metaHeader().createdAtUnixMs == firstHeader.createdAtUnixMs);
}

TEST_CASE("MusicLibrary rejects unsupported library versions", "[core][library]")
{
  auto temp = TempDir{};
  auto env = rs::lmdb::Environment{temp.path(), {.flags = MDB_NOTLS, .maxDatabases = 8}};

  auto txn = rs::lmdb::WriteTransaction{env};
  auto metaStore = rs::core::LibraryMetaStore{rs::lmdb::Database{txn, "meta"}};
  auto header = rs::core::LibraryMetaHeader{.magic = rs::core::kLibraryMetaMagic,
                                            .libraryVersion = rs::core::kLibraryVersion + 1,
                                            .flags = 0,
                                            .createdAtUnixMs = 1,
                                            .migratedAtUnixMs = 1,
                                            .libraryId = {}};
  metaStore.create(txn, header);
  txn.commit();

  REQUIRE_THROWS_AS(rs::core::MusicLibrary{temp.path()}, rs::Exception);
}
