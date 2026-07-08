// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "test/unit/lmdb/TestUtils.h"
#include <ao/Error.h>
#include <ao/library/Meta.h>
#include <ao/library/MetaStore.h>
#include <ao/lmdb/Database.h>
#include <ao/lmdb/Environment.h>
#include <ao/lmdb/Transaction.h>

#include <catch2/catch_test_macros.hpp>
#include <lmdb.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ao::library::test
{
  using namespace ao::lmdb;
  using namespace ao::lmdb::test;

  TEST_CASE("MetaStore - load returns CorruptData for invalid metadata header size", "[library][unit][meta-store]")
  {
    auto temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});
    auto wtxn = beginWriteTransaction(env);
    auto db = openDatabase(wtxn, "meta");
    auto store = MetaStore{db};

    // Write an invalid sized struct (e.g. 1 byte) directly to the DB to simulate corruption or older version
    auto writer = db.writer(wtxn);
    auto invalidData = std::vector{std::byte{0x42}};
    REQUIRE(writer.create(static_cast<std::uint32_t>(MetaRecordId::Header), std::span<std::byte const>{invalidData}));
    REQUIRE(wtxn.commit());

    auto rtxn = beginReadTransaction(env);
    auto const result = store.load(rtxn);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == Error::Code::CorruptData);
  }

  TEST_CASE("MetaStore - creates and loads metadata header", "[library][unit][meta-store]")
  {
    auto temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});
    auto wtxn = beginWriteTransaction(env);
    auto db = openDatabase(wtxn, "meta");
    auto store = MetaStore{db};

    auto header = MetaHeader{.magic = 0xDEADBEEF,
                             .libraryVersion = 42,
                             .flags = 0,
                             .createdTime = std::chrono::sys_time{std::chrono::milliseconds{1234567890}},
                             .libraryId = {}};
    store.create(wtxn, header);
    REQUIRE(wtxn.commit());

    auto rtxn = beginReadTransaction(env);
    auto const loadedResult = store.load(rtxn);
    REQUIRE(loadedResult);
    CHECK(loadedResult->magic == 0xDEADBEEF);
    CHECK(loadedResult->libraryVersion == 42);
    CHECK(loadedResult->createdTime.time_since_epoch().count() == 1234567890);
  }

  TEST_CASE("MetaStore - load returns NotFound for missing header", "[library][unit][meta-store]")
  {
    auto temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});
    auto wtxn = beginWriteTransaction(env);
    auto db = openDatabase(wtxn, "meta");
    auto store = MetaStore{db};
    REQUIRE(wtxn.commit());

    auto rtxn = beginReadTransaction(env);
    auto const result = store.load(rtxn);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == Error::Code::NotFound);
  }

  TEST_CASE("MetaStore - update overwrites previous header values", "[library][unit][meta-store]")
  {
    auto temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});
    auto wtxn = beginWriteTransaction(env);
    auto db = openDatabase(wtxn, "meta");
    auto store = MetaStore{db};

    auto header = MetaHeader{.magic = 0xAAAAAAAA,
                             .libraryVersion = 1,
                             .flags = 0,
                             .createdTime = std::chrono::sys_time{std::chrono::milliseconds{100}},
                             .libraryId = {}};
    store.create(wtxn, header);
    REQUIRE(wtxn.commit());

    auto wtxn2 = beginWriteTransaction(env);
    header.libraryVersion = 2;
    store.update(wtxn2, header);
    REQUIRE(wtxn2.commit());

    auto rtxn = beginReadTransaction(env);
    auto const loadedResult = store.load(rtxn);
    REQUIRE(loadedResult);
    CHECK(loadedResult->libraryVersion == 2);
  }
} // namespace ao::library::test
