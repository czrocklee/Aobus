// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/lmdb/TestUtils.h"
#include <ao/Error.h>
#include <ao/library/Meta.h>
#include <ao/library/MetaStore.h>
#include <ao/library/MusicLibrary.h>
#include <ao/lmdb/Environment.h>
#include <ao/lmdb/Transaction.h>

#include <catch2/catch_test_macros.hpp>
#include <lmdb.h>

#include <chrono>
#include <cstdint>
#include <filesystem>

namespace ao::library::test
{
  using namespace ao::lmdb;
  using namespace ao::lmdb::test;

  namespace
  {
    void createLibraryMetaHeader(std::filesystem::path const& path, std::uint32_t libraryVersion)
    {
      auto env = lmdb::test::openEnvironment(path, {.flags = MDB_NOTLS, .maxDatabases = 8});
      auto txn = lmdb::test::beginWriteTransaction(env);
      auto metaStore = MetaStore{lmdb::test::openDatabase(txn, "meta")};
      auto header = MetaHeader{.magic = kLibraryMetaMagic,
                               .libraryVersion = libraryVersion,
                               .flags = 0,
                               .createdTime = std::chrono::sys_time{std::chrono::milliseconds{1}},
                               .libraryId = {}};
      metaStore.create(txn, header);
      REQUIRE(txn.commit());
    }
  } // namespace

  TEST_CASE("MusicLibrary - initializes metadata header", "[library][unit][music-library]")
  {
    auto const temp = ao::test::TempDir{};

    auto firstResult = MusicLibrary::open(temp.path(), temp.path());
    REQUIRE(firstResult);
    auto const& first = *firstResult;
    auto const firstHeader = MetaHeader{first.metaHeader()};

    CHECK(firstHeader.magic == kLibraryMetaMagic);
    CHECK(firstHeader.libraryVersion == kLibraryVersion);

    auto reopenedResult = MusicLibrary::open(temp.path(), temp.path());
    REQUIRE(reopenedResult);
    auto const& reopened = *reopenedResult;
    CHECK(reopened.metaHeader().libraryId == firstHeader.libraryId);
    CHECK(reopened.metaHeader().createdTime == firstHeader.createdTime);
  }

  TEST_CASE("MusicLibrary - reports unsupported library versions as CorruptData", "[library][unit][music-library]")
  {
    auto const temp = ao::test::TempDir{};
    constexpr std::uint32_t kLegacyV1LibraryVersion = 1;
    constexpr std::uint32_t kPreviousColdLayoutLibraryVersion = 2;

    SECTION("future version")
    {
      createLibraryMetaHeader(temp.path(), kLibraryVersion + 1);

      auto const result = MusicLibrary::open(temp.path(), temp.path());
      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::CorruptData);
    }

    SECTION("old version")
    {
      static_assert(kLegacyV1LibraryVersion != kLibraryVersion);
      createLibraryMetaHeader(temp.path(), kLegacyV1LibraryVersion);

      auto const result = MusicLibrary::open(temp.path(), temp.path());
      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::CorruptData);
    }

    SECTION("previous cold layout version")
    {
      static_assert(kPreviousColdLayoutLibraryVersion != kLibraryVersion);
      createLibraryMetaHeader(temp.path(), kPreviousColdLayoutLibraryVersion);

      auto const result = MusicLibrary::open(temp.path(), temp.path());
      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::CorruptData);
    }
  }

  TEST_CASE("MusicLibrary - accessors return valid references", "[library][unit][music-library]")
  {
    auto const temp = ao::test::TempDir{};
    auto const ml = MusicLibrary{temp.path(), temp.path()};

    // All store accessors should be callable without crashing
    CHECK_NOTHROW(ml.tracks());
    CHECK_NOTHROW(ml.lists());
    CHECK_NOTHROW(ml.resources());
    CHECK_NOTHROW(ml.dictionary());
    CHECK_NOTHROW(ml.manifest());
    CHECK(ml.rootPath() == temp.path());
  }

  TEST_CASE("MusicLibrary - read and write transactions work", "[library][unit][music-library]")
  {
    auto const temp = ao::test::TempDir{};
    auto ml = MusicLibrary{temp.path(), temp.path()};

    auto wtxn = ml.writeTransaction();
    CHECK_NOTHROW(wtxn.commit());

    auto rtxn = ml.readTransaction(); // validates read access to the database
  }
} // namespace ao::library::test
