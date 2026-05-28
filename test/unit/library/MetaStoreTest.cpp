// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "ao/library/MetaStore.h"

#include "ao/Exception.h"
#include "ao/library/Meta.h"
#include "ao/lmdb/Database.h"
#include "ao/lmdb/Environment.h"
#include "ao/lmdb/Transaction.h"
#include "test/unit/lmdb/TestUtils.h"

#include <catch2/catch_test_macros.hpp>
#include <lmdb.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ao::library::test
{
  using namespace ao::lmdb;
  using namespace ao::lmdb::test;

  TEST_CASE("MetaStore - Invalid metadata header size throws", "[library][unit][meta_store]")
  {
    auto temp = TempDir{};
    auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};
    auto wtxn = WriteTransaction{env};
    auto db = Database{wtxn, "meta"};
    auto store = MetaStore{db};

    // Write an invalid sized struct (e.g. 1 byte) directly to the DB to simulate corruption or older version
    auto writer = db.writer(wtxn);
    auto invalidData = std::vector{std::byte{0x42}};
    writer.create(static_cast<std::uint32_t>(MetaRecordId::Header), std::span<std::byte const>{invalidData});
    wtxn.commit();

    auto rtxn = ReadTransaction{env};
    REQUIRE_THROWS_AS(store.load(rtxn), ao::Exception);
  }

  TEST_CASE("MetaStore - Create and load header", "[library][unit][meta_store]")
  {
    auto temp = TempDir{};
    auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};
    auto wtxn = WriteTransaction{env};
    auto db = Database{wtxn, "meta"};
    auto store = MetaStore{db};

    auto header =
      MetaHeader{.magic = 0xDEADBEEF, .libraryVersion = 42, .flags = 0, .createdAtUnixMs = 1234567890, .libraryId = {}};
    store.create(wtxn, header);
    wtxn.commit();

    auto rtxn = ReadTransaction{env};
    auto const optLoaded = store.load(rtxn);
    REQUIRE(optLoaded.has_value());
    CHECK(optLoaded->magic == 0xDEADBEEF);
    CHECK(optLoaded->libraryVersion == 42);
    CHECK(optLoaded->createdAtUnixMs == 1234567890);
  }

  TEST_CASE("MetaStore - Update header overwrites previous values", "[library][unit][meta_store]")
  {
    auto temp = TempDir{};
    auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};
    auto wtxn = WriteTransaction{env};
    auto db = Database{wtxn, "meta"};
    auto store = MetaStore{db};

    auto header =
      MetaHeader{.magic = 0xAAAAAAAA, .libraryVersion = 1, .flags = 0, .createdAtUnixMs = 100, .libraryId = {}};
    store.create(wtxn, header);
    wtxn.commit();

    auto wtxn2 = WriteTransaction{env};
    header.libraryVersion = 2;
    store.update(wtxn2, header);
    wtxn2.commit();

    auto rtxn = ReadTransaction{env};
    auto const optLoaded = store.load(rtxn);
    REQUIRE(optLoaded.has_value());
    CHECK(optLoaded->libraryVersion == 2);
  }
} // namespace ao::library::test
