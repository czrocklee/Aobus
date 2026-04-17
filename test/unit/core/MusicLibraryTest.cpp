// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <catch2/catch.hpp>

#include <rs/Exception.h>
#include <rs/core/LibraryMeta.h>
#include <rs/core/LibraryMetaStore.h>
#include <rs/core/MusicLibrary.h>
#include <rs/core/ListLayout.h>
#include <rs/core/TrackLayout.h>
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
  REQUIRE(firstHeader.headerVersion == rs::core::kLibraryMetaHeaderVersion);
  REQUIRE(firstHeader.librarySchemaVersion == rs::core::kLibrarySchemaVersion);
  REQUIRE(firstHeader.trackLayoutVersion == rs::core::kTrackLayoutVersion);
  REQUIRE(firstHeader.listLayoutVersion == rs::core::kListLayoutVersion);

  auto const reopened = rs::core::MusicLibrary{temp.path()};
  REQUIRE(reopened.metaHeader().libraryId == firstHeader.libraryId);
  REQUIRE(reopened.metaHeader().createdAtUnixMs == firstHeader.createdAtUnixMs);
}

TEST_CASE("MusicLibrary rejects unsupported metadata versions", "[core][library]")
{
  auto temp = TempDir{};
  auto env = rs::lmdb::Environment{temp.path(), {.flags = MDB_NOTLS, .maxDatabases = 8}};

  auto txn = rs::lmdb::WriteTransaction{env};
  auto metaStore = rs::core::LibraryMetaStore{rs::lmdb::Database{txn, "meta"}};
  auto header = rs::core::LibraryMetaHeader{.magic = rs::core::kLibraryMetaMagic,
                                            .headerVersion = rs::core::kLibraryMetaHeaderVersion,
                                            .librarySchemaVersion = static_cast<std::uint16_t>(rs::core::kLibrarySchemaVersion + 1),
                                            .trackLayoutVersion = rs::core::kTrackLayoutVersion,
                                            .listLayoutVersion = rs::core::kListLayoutVersion,
                                            .flags = 0,
                                            .createdAtUnixMs = 1,
                                            .migratedAtUnixMs = 1,
                                            .libraryId = {}};
  metaStore.create(txn, header);
  txn.commit();

  REQUIRE_THROWS_AS(rs::core::MusicLibrary{temp.path()}, rs::Exception);
}
