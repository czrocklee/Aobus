// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "test/unit/lmdb/TestUtils.h"
#include <ao/Error.h>
#include <ao/library/FileManifestBuilder.h>
#include <ao/library/FileManifestLayout.h>
#include <ao/library/FileManifestStore.h>
#include <ao/lmdb/Database.h>
#include <ao/lmdb/Environment.h>
#include <ao/lmdb/Transaction.h>

#include <catch2/catch_test_macros.hpp>
#include <lmdb.h>

#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace ao::library::test
{
  using namespace ao::lmdb;
  using namespace ao::lmdb::test;

  TEST_CASE("FileManifestStore - create returns ValueTooLarge for invalid URI length", "[library][unit][manifest]")
  {
    auto temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});
    auto wtxn = beginWriteTransaction(env);
    auto db = openDatabase(wtxn, "manifests");
    auto store = FileManifestStore{db};
    auto writer = store.writer(wtxn);

    auto const longUri = std::string(501, 'a');

    auto const result = writer.put(longUri, std::span<std::byte const>{});
    REQUIRE_FALSE(result);
    CHECK(result.error().code == Error::Code::ValueTooLarge);
  }

  TEST_CASE("FileManifestStore - writes and reads back manifests", "[library][unit][manifest]")
  {
    auto temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});
    auto wtxn = beginWriteTransaction(env);
    auto db = openDatabase(wtxn, "manifests");
    auto store = FileManifestStore{db};

    auto builder = FileManifestBuilder::createNew();
    builder.trackId(TrackId{42}).fileSize(12345).mtime(67890).status(FileStatus::Available);
    auto const payload = builder.serialize();

    REQUIRE(store.writer(wtxn).put("song.flac", payload));
    REQUIRE(wtxn.commit());

    auto rtxn = beginReadTransaction(env);
    auto const viewResult = store.reader(rtxn).get("song.flac");
    REQUIRE(viewResult);
    CHECK(viewResult->trackId() == TrackId{42});
    CHECK(viewResult->fileSize() == 12345);
    CHECK(viewResult->mtime() == 67890);
    CHECK(viewResult->status() == FileStatus::Available);
  }

  TEST_CASE("FileManifestStore - get returns NotFound for missing URI", "[library][unit][manifest]")
  {
    auto temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});
    auto wtxn = beginWriteTransaction(env);
    auto db = openDatabase(wtxn, "manifests");
    auto store = FileManifestStore{db};
    REQUIRE(wtxn.commit());

    auto rtxn = beginReadTransaction(env);
    auto const viewResult = store.reader(rtxn).get("nonexistent.flac");
    REQUIRE_FALSE(viewResult);
    CHECK(viewResult.error().code == Error::Code::NotFound);
  }

  TEST_CASE("FileManifestStore - remove is idempotent", "[library][unit][manifest]")
  {
    auto temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});
    auto wtxn = beginWriteTransaction(env);
    auto db = openDatabase(wtxn, "manifests");
    auto store = FileManifestStore{db};

    auto builder = FileManifestBuilder::createNew();
    builder.trackId(TrackId{42}).fileSize(12345).mtime(67890).status(FileStatus::Available);

    auto writer = store.writer(wtxn);
    REQUIRE(writer.put("song.flac", builder.serialize()));
    REQUIRE(writer.remove("song.flac"));
    REQUIRE(writer.remove("song.flac"));
    REQUIRE(wtxn.commit());

    auto rtxn = beginReadTransaction(env);
    auto const viewResult = store.reader(rtxn).get("song.flac");
    REQUIRE_FALSE(viewResult);
    CHECK(viewResult.error().code == Error::Code::NotFound);
  }

  TEST_CASE("FileManifestStore - read returns CorruptData for corrupt entries", "[library][unit][manifest]")
  {
    auto temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});
    auto wtxn = beginWriteTransaction(env);
    auto db = openDatabase(wtxn, "manifests");
    auto store = FileManifestStore{db};
    auto invalidPayload = std::vector{std::byte{0x42}};

    REQUIRE(store.writer(wtxn).put("song.flac", invalidPayload));
    REQUIRE(wtxn.commit());

    auto rtxn = beginReadTransaction(env);
    auto const result = store.reader(rtxn).get("song.flac");
    REQUIRE_FALSE(result);
    CHECK(result.error().code == Error::Code::CorruptData);
  }
} // namespace ao::library::test
