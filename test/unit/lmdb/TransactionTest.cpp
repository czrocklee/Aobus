// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "test/unit/TestUtils.h"
#include "test/unit/lmdb/LmdbTestSupport.h"
#include <ao/Exception.h>
#include <ao/lmdb/Database.h>
#include <ao/lmdb/Environment.h>
#include <ao/lmdb/Transaction.h>

#include <catch2/catch_test_macros.hpp>
#include <lmdb.h>

#include <string_view>
#include <utility>

namespace ao::lmdb::test
{
  TEST_CASE("ReadTransaction - helper starts transaction", "[lmdb][unit][transaction]")
  {
    auto temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    // First create database with write transaction
    auto wtxn = beginWriteTransaction(env);
    auto db = openDatabase(wtxn, "test");
    REQUIRE(wtxn.commit());

    // Then use read transaction
    auto txn = beginReadTransaction(env);
    auto reader = db.reader(txn);
    CHECK(reader.begin() == reader.end()); // Empty DB
  }

  TEST_CASE("ReadTransaction - begin returns transaction", "[lmdb][unit][transaction]")
  {
    auto temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto txn = ReadTransaction::begin(env);

    CHECK(txn);
  }

  TEST_CASE("ReadTransaction - destructor aborts", "[lmdb][unit][transaction]")
  {
    auto temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    // Create database with write transaction
    auto wtxn = beginWriteTransaction(env);
    auto db = openDatabase(wtxn, "test");
    REQUIRE(wtxn.commit());

    // Read transaction - destructor should abort
    {
      auto txn = beginReadTransaction(env);
      // Destructor should abort
    }

    // Should be able to start new transaction
    auto txn2 = beginReadTransaction(env);
    auto reader = db.reader(txn2);
    CHECK(reader.begin() == reader.end());
  }

  TEST_CASE("ReadTransaction - move constructor transfers usable transactions", "[lmdb][unit][transaction]")
  {
    auto temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    // Create database first with write transaction
    auto wtxn = beginWriteTransaction(env);
    auto db = openDatabase(wtxn, "test");
    REQUIRE(wtxn.commit());

    auto txn1 = beginReadTransaction(env);
    auto txn2 = ReadTransaction{std::move(txn1)};
    // Verify moved transaction is valid by using it
    auto reader = db.reader(txn2);
    CHECK(reader.begin() == reader.end());
  }

  // ============================================================================
  // WriteTransaction Tests
  // ============================================================================
  TEST_CASE("WriteTransaction - helper starts transaction", "[lmdb][unit][transaction]")
  {
    auto temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto txn = beginWriteTransaction(env);
    // Verify transaction is valid by using it to create a database
    auto db = openDatabase(txn, "test");
    [[maybe_unused]] auto writer = db.writer(txn);
  }

  TEST_CASE("WriteTransaction - begin returns transaction", "[lmdb][unit][transaction]")
  {
    auto temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto txn = WriteTransaction::begin(env);

    CHECK(txn);
  }

  TEST_CASE("WriteTransaction - commit persists written data", "[lmdb][unit][transaction]")
  {
    auto temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    // Create database, write data, commit
    auto wtxn = beginWriteTransaction(env);
    auto db = openDatabase(wtxn, "test");
    auto writer = db.writer(wtxn);
    REQUIRE(writer.create(1, createStringData("test data")));
    CHECK(wtxn.isActive());
    CHECK_FALSE(wtxn.isFinished());
    REQUIRE(wtxn.commit());
    CHECK_FALSE(wtxn.isActive());
    CHECK(wtxn.isFinished());

    // Start a new transaction - should work now
    auto wtxn2 = beginWriteTransaction(env);
    auto reader = db.reader(wtxn2);
    auto it = reader.begin();
    REQUIRE(it != reader.end());
    REQUIRE(it->first == 1);
  }

  TEST_CASE("WriteTransaction - destructor without commit aborts", "[lmdb][unit][transaction]")
  {
    auto temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    // Create database
    auto dbTxn = beginWriteTransaction(env);
    auto db = openDatabase(dbTxn, "test");
    REQUIRE(dbTxn.commit());

    // Write data without committing - transaction should abort on destruction
    {
      auto txn = beginWriteTransaction(env);
      auto writer = db.writer(txn);
      REQUIRE(writer.create(1, createStringData("uncommitted")));
      // Without commit, transaction aborts on destruction
    }

    // Data should not be visible
    auto txn = beginReadTransaction(env);
    auto reader = db.reader(txn);
    REQUIRE_FALSE(reader.get(1).has_value());
  }

  TEST_CASE("WriteTransaction - explicit abort is terminal and idempotent", "[lmdb][unit][transaction]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});
    auto transaction = beginWriteTransaction(env);
    auto db = openDatabase(transaction, "test");
    auto writer = db.writer(transaction);
    REQUIRE(writer.create(1, createStringData("aborted")));

    transaction.abort();
    CHECK_FALSE(transaction.isActive());
    CHECK(transaction.isFinished());
    CHECK_NOTHROW(transaction.abort());

    auto const commitResult = transaction.commit();
    REQUIRE_FALSE(commitResult);
    CHECK(commitResult.error().code == Error::Code::InvalidState);
    CHECK_THROWS_AS(writer.get(1), Exception);
  }

  TEST_CASE("WriteTransaction - move constructor transfers usable transactions", "[lmdb][unit][transaction]")
  {
    auto temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    // First create the database
    auto wtxn = beginWriteTransaction(env);
    auto db = openDatabase(wtxn, "test");
    REQUIRE(wtxn.commit());

    // Now test move
    auto txn1 = beginWriteTransaction(env);
    auto txn2 = WriteTransaction{std::move(txn1)};
    // Verify moved transaction is valid by using it
    [[maybe_unused]] auto writer = db.writer(txn2);
    REQUIRE(txn2.commit());
  }

  // ============================================================================
  // Nested Transaction Tests
  // ============================================================================
  TEST_CASE("NestedTransaction - child commit merges to parent", "[lmdb][unit][nested]")
  {
    auto temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    // Create parent write transaction and open database
    auto parentTxn = beginWriteTransaction(env);
    auto db = openDatabase(parentTxn, "test");

    // Create nested write transaction
    auto childTxn = beginWriteTransaction(parentTxn);
    auto writer = db.writer(childTxn);
    REQUIRE(writer.create(1, createStringData("nested data")));

    // Commit child - merges to parent
    REQUIRE(childTxn.commit());
    // Parent should still be valid
    REQUIRE(parentTxn.commit());

    // Verify data is visible
    auto rtxn = beginReadTransaction(env);
    auto reader = db.reader(rtxn);
    auto it = reader.begin();
    REQUIRE(it != reader.end());
    REQUIRE(it->first == 1);
  }

  TEST_CASE("NestedTransaction - begin rejects a finished parent", "[lmdb][unit][nested]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});
    auto parent = beginWriteTransaction(env);
    REQUIRE(parent.commit());

    auto const childResult = WriteTransaction::begin(parent);

    REQUIRE_FALSE(childResult);
    CHECK(childResult.error().code == Error::Code::InvalidState);
  }

  TEST_CASE("NestedTransaction - child abort does not affect parent", "[lmdb][unit][nested]")
  {
    auto temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    // Create parent transaction and open database
    auto parentTxn = beginWriteTransaction(env);
    auto db = openDatabase(parentTxn, "test");

    // Create child transaction and write data
    {
      auto childTxn = beginWriteTransaction(parentTxn);
      auto writer = db.writer(childTxn);
      REQUIRE(writer.create(1, createStringData("child data")));
      // Child transaction aborts on destruction without commit
    }

    // Parent commits - child data should NOT be included
    REQUIRE(parentTxn.commit());

    // Verify data is NOT visible
    auto rtxn = beginReadTransaction(env);
    auto reader = db.reader(rtxn);
    REQUIRE_FALSE(reader.get(1).has_value());
  }
} // namespace ao::lmdb::test
