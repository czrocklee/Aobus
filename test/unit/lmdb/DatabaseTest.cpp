// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "test/unit/TestUtils.h"
#include "test/unit/lmdb/LmdbTestSupport.h"
#include <ao/Error.h>
#include <ao/Exception.h>
#include <ao/lmdb/Database.h>
#include <ao/lmdb/Environment.h>

#include <catch2/catch_test_macros.hpp>
#include <lmdb.h>

#include <utility>

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

  TEST_CASE("Database - write open rejects a finished transaction", "[lmdb][unit][database]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});
    auto transaction = beginWriteTransaction(env);
    REQUIRE(transaction.commit());

    auto const result = Database::open(transaction, "finished");

    REQUIRE_FALSE(result);
    CHECK(result.error().code == Error::Code::InvalidState);
  }

  TEST_CASE("Database - read-only open rejects a moved-from transaction", "[lmdb][unit][database]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});
    auto setup = beginWriteTransaction(env);
    auto db = openDatabase(setup, "test");
    REQUIRE(setup.commit());

    auto source = beginReadTransaction(env);
    auto destination = ReadTransaction{std::move(source)};
    // ReadTransaction specifies an inactive moved-from state.
    auto const result = Database::open(source, "test");

    REQUIRE_FALSE(result);
    CHECK(result.error().code == Error::Code::InvalidState);
    CHECK(db.reader(destination).begin() == db.reader(destination).end());
  }

  TEST_CASE("Database - reader rejects a moved-from transaction", "[lmdb][unit][database]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});
    auto setup = beginWriteTransaction(env);
    auto db = openDatabase(setup, "test");
    REQUIRE(setup.commit());

    auto source = beginReadTransaction(env);
    auto destination = ReadTransaction{std::move(source)};

    // ReadTransaction specifies an inactive moved-from state.
    // NOLINTNEXTLINE(bugprone-use-after-move)
    CHECK_THROWS_AS(db.reader(source), Exception);
    CHECK(db.reader(destination).begin() == db.reader(destination).end());
  }
} // namespace ao::lmdb::test
