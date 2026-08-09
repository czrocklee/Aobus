// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/lmdb/Database.h>

#include "lib/lmdb/detail/TransactionFailure.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/lmdb/LmdbTestSupport.h"
#include <ao/Error.h>
#include <ao/lmdb/Environment.h>

#include <catch2/catch_test_macros.hpp>
#include <lmdb.h>

#include <optional>
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

    auto dbRes = Database::open(wtxn, "newdb");

    CHECK(dbRes);
    REQUIRE(wtxn.commit());
  }

  TEST_CASE("Database - read-only open returns NotFound for missing database", "[lmdb][unit][database]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});
    auto txn = beginReadTransaction(env);

    auto dbRes = Database::open(txn, "missing");

    CHECK_FALSE(dbRes);
    CHECK(dbRes.error().code == Error::Code::NotFound);
  }

  TEST_CASE("Database - openExisting never creates a missing named database", "[lmdb][unit][database]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});
    auto transaction = beginWriteTransaction(env);

    auto missingRes = Database::openExisting(transaction, "missing");

    REQUIRE_FALSE(missingRes);
    CHECK(missingRes.error().code == Error::Code::NotFound);
    CHECK(Database::main(transaction).reader(transaction).entryCount() == 0);
    REQUIRE(transaction.commit());
  }

  TEST_CASE("Database - openExisting validates exact native key flags", "[lmdb][unit][database]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});
    auto transaction = beginWriteTransaction(env);
    REQUIRE(Database::open(transaction, "integer", Database::KeyKind::Integer));
    REQUIRE(Database::open(transaction, "blob", Database::KeyKind::Blob));

    CHECK(Database::openExisting(transaction, "integer", Database::KeyKind::Integer));
    CHECK(Database::openExisting(transaction, "blob", Database::KeyKind::Blob));
    auto wrongIntegerRes = Database::openExisting(transaction, "integer", Database::KeyKind::Blob);
    auto wrongBlobRes = Database::openExisting(transaction, "blob", Database::KeyKind::Integer);
    REQUIRE_FALSE(wrongIntegerRes);
    REQUIRE_FALSE(wrongBlobRes);
    CHECK(wrongIntegerRes.error().code == Error::Code::CorruptData);
    CHECK(wrongBlobRes.error().code == Error::Code::CorruptData);
  }

  TEST_CASE("Database - openExisting rejects an ordinary main-database row", "[lmdb][unit][database]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});
    auto transaction = beginWriteTransaction(env);
    auto main = Database::main(transaction);
    REQUIRE(main.writer(transaction).create(createStringData("ordinary"), createStringData("value")));

    auto ordinaryRes = Database::openExisting(transaction, "ordinary");

    REQUIRE_FALSE(ordinaryRes);
    CHECK(ordinaryRes.error().code == Error::Code::CorruptData);
  }

  TEST_CASE("Database - failed write open unwinds and rolls back database creation", "[lmdb][regression][database]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 1});
    auto optFailure = std::optional<Error>{};

    try
    {
      auto transaction = beginWriteTransaction(env);
      REQUIRE(Database::open(transaction, "first"));
      std::ignore = Database::open(transaction, "second");
      FAIL("opening a database beyond maxdbs should throw");
    }
    catch (detail::TransactionFailure const& transactionFailure)
    {
      optFailure = transactionFailure.error();
    }

    REQUIRE(optFailure);
    CHECK(optFailure->code == Error::Code::IoError);

    auto readTransaction = beginReadTransaction(env);
    auto firstRes = Database::open(readTransaction, "first");
    REQUIRE_FALSE(firstRes);
    CHECK(firstRes.error().code == Error::Code::NotFound);
  }
} // namespace ao::lmdb::test
