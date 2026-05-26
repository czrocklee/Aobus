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

using namespace ao::library;
using namespace ao::lmdb;
using namespace ao::lmdb::test;

TEST_CASE("MetaStore - Invalid metadata header size throws", "[library][meta_store]")
{
  auto temp = TempDir{};
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};
  auto wtxn = WriteTransaction{env};
  auto db = Database{wtxn, "meta"};
  auto store = MetaStore{db};

  // Write an invalid sized struct (e.g. 1 byte) directly to the DB to simulate corruption or older version
  auto writer = db.writer(wtxn);
  auto invalidData = std::vector<std::byte>{std::byte{0x42}};
  writer.create(static_cast<std::uint32_t>(MetaRecordId::Header), std::span<std::byte const>{invalidData});
  wtxn.commit();

  auto rtxn = ReadTransaction{env};
  REQUIRE_THROWS_AS(store.load(rtxn), ao::Exception);
}
