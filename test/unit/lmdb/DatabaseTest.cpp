// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "test/unit/lmdb/TestUtils.h"
#include <ao/lmdb/Database.h>
#include <ao/lmdb/Environment.h>
#include <ao/lmdb/Transaction.h>

#include <catch2/catch_test_macros.hpp>
#include <lmdb.h>

namespace ao::lmdb::test
{
  TEST_CASE("Database - helper opens database", "[lmdb][unit][database]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto db = openDatabase(wtxn, "newdb");
    REQUIRE(wtxn.commit());

    auto const txn = beginReadTransaction(env);
    auto const reader = db.reader(txn);
    CHECK(reader.begin() == reader.end());
  }

  TEST_CASE("Database - open returns database", "[lmdb][unit][database]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});
    auto wtxn = beginWriteTransaction(env);

    auto db = Database::open(wtxn, "newdb");

    CHECK(db);
    REQUIRE(wtxn.commit());
  }

  TEST_CASE("Database - read-only open returns NotFound for missing database", "[lmdb][unit][database]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});
    auto txn = beginReadTransaction(env);

    auto db = Database::open(txn, "missing");

    CHECK_FALSE(db);
    CHECK(db.error().code == Error::Code::NotFound);
  }
} // namespace ao::lmdb::test
