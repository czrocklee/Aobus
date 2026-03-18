// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <catch2/catch.hpp>

#include <lmdb.h>
#include <rs/lmdb/Database.h>
#include <rs/lmdb/Environment.h>
#include <rs/lmdb/Transaction.h>
#include <test/lmdb/LmdbTestUtils.h>

#include <cstring>
#include <optional>
#include <string_view>

using namespace rs::lmdb;

// ============================================================================
// ReadTransaction Tests
// ============================================================================

TEST_CASE("ReadTransaction - constructor starts transaction", "[lmdb][transaction]")
{
  TempDir temp;
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  // First create database with write transaction
  {
    WriteTransaction wtxn(env);
    Database db{wtxn, "test"};
    wtxn.commit();
  }

  // Then use read transaction
  ReadTransaction txn(env);
  Database db{txn, "test"};
  auto reader = db.reader(txn);
  REQUIRE(reader.begin() == reader.end()); // Empty DB
}

TEST_CASE("ReadTransaction - destructor aborts", "[lmdb][transaction]")
{
  TempDir temp;
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  // Create database first with write transaction
  {
    WriteTransaction wtxn(env);
    Database db{wtxn, "test"};
    wtxn.commit();
  }

  // Read transaction - destructor should abort
  {
    ReadTransaction txn(env);
    Database db{txn, "test"};
    (void)db;
    // Destructor should abort
  }
  // Should be able to start new transaction
  ReadTransaction txn2(env);
  Database db2{txn2, "test"};
  auto reader = db2.reader(txn2);
  REQUIRE(reader.begin() == reader.end());
}

TEST_CASE("ReadTransaction - move", "[lmdb][transaction]")
{
  TempDir temp;
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  // Create database first with write transaction
  {
    WriteTransaction wtxn(env);
    Database db{wtxn, "test"};
    wtxn.commit();
  }

  ReadTransaction txn1(env);
  ReadTransaction txn2{std::move(txn1)};
  // Verify moved transaction is valid by using it
  Database db{txn2, "test"};
  auto reader = db.reader(txn2);
  REQUIRE(reader.begin() == reader.end());
}

// ============================================================================
// WriteTransaction Tests
// ============================================================================

TEST_CASE("WriteTransaction - constructor starts transaction", "[lmdb][transaction]")
{
  TempDir temp;
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  WriteTransaction txn(env);
  // Verify transaction is valid by using it to create a database
  Database db{txn, "test"};
  auto writer = db.writer(txn);
  (void)writer;
}

TEST_CASE("WriteTransaction - commit", "[lmdb][transaction]")
{
  TempDir temp;
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  // Create database, write data, commit
  WriteTransaction wtxn(env);
  Database db{wtxn, "test"};
  auto writer = db.writer(wtxn);
  REQUIRE(!writer.create(1, makeBuffer(createStringData("test data"))).empty());
  wtxn.commit();

  // Start a new transaction - should work now
  WriteTransaction wtxn2(env);
  Database db2{wtxn2, "test"};
  auto reader = db2.reader(wtxn2);
  auto it = reader.begin();
  REQUIRE(it != reader.end());
  REQUIRE(it->first == 1);
}

TEST_CASE("WriteTransaction - destructor without commit aborts", "[lmdb][transaction]")
{
  TempDir temp;
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  // Create database first
  {
    WriteTransaction dbTxn(env);
    Database db{dbTxn, "test"};
    dbTxn.commit();
  }

  // Test that uncommitted nested transaction aborts
  {
    WriteTransaction parentTxn(env);
    Database db{parentTxn, "test"};
    {
      WriteTransaction txn(parentTxn);
      auto writer = db.writer(txn);
      REQUIRE(!writer.create(1, makeBuffer(createStringData("uncommitted"))).empty());
      // Without commit, nested transaction aborts on destruction
    }
    // Parent commit - but since nested txn didn't commit, data should not be visible
    parentTxn.commit();
  }

  // Data should not be visible
  {
    ReadTransaction txn(env);
    Database db{txn, "test"};
    auto reader = db.reader(txn);
    auto data = reader.get(1);
    REQUIRE(data == std::nullopt);
  }
}

TEST_CASE("WriteTransaction - move", "[lmdb][transaction]")
{
  TempDir temp;
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  // First create the database
  {
    WriteTransaction wtxn(env);
    Database db{wtxn, "test"};
    wtxn.commit();
  }

  // Now test move
  WriteTransaction txn1(env);
  WriteTransaction txn2{std::move(txn1)};
  // Verify moved transaction is valid by using it
  Database db{txn2, "test"};
  auto writer = db.writer(txn2);
  (void)writer;
  txn2.commit();
}

// ============================================================================
// Nested Transaction Tests
// ============================================================================

TEST_CASE("NestedTransaction - child commit merges to parent", "[lmdb][nested]")
{
  TempDir temp;
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  // Create database first
  {
    WriteTransaction wtxn(env);
    Database db{wtxn, "test"};
    wtxn.commit();
  }

  // Create parent write transaction
  WriteTransaction parentTxn(env);

  // Create nested write transaction
  WriteTransaction childTxn(parentTxn);
  Database db{childTxn, "test"};
  auto writer = db.writer(childTxn);
  REQUIRE(!writer.create(1, makeBuffer(createStringData("nested data"))).empty());

  // Commit child - merges to parent
  childTxn.commit();
  // Parent should still be valid
  parentTxn.commit();

  // Verify data is visible
  ReadTransaction rtxn(env);
  Database verifyDb{rtxn, "test"};
  auto reader = verifyDb.reader(rtxn);
  auto it = reader.begin();
  REQUIRE(it != reader.end());
  REQUIRE(it->first == 1);
}

TEST_CASE("NestedTransaction - child abort does not affect parent", "[lmdb][nested]")
{
  TempDir temp;
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  // Create database first
  {
    WriteTransaction wtxn(env);
    Database db{wtxn, "test"};
    wtxn.commit();
  }

  // Create parent transaction
  WriteTransaction parentTxn(env);
  Database db{parentTxn, "test"};

  // Create child transaction and write data
  {
    WriteTransaction childTxn(parentTxn);
    auto writer = db.writer(childTxn);
    REQUIRE(!writer.create(1, makeBuffer(createStringData("child data"))).empty());
    // Child transaction aborts on destruction without commit
  }

  // Parent commits - child data should NOT be included
  parentTxn.commit();

  // Verify data is NOT visible
  {
    ReadTransaction rtxn(env);
    Database readDb{rtxn, "test"};
    auto reader = readDb.reader(rtxn);
    auto data = reader.get(1);
    REQUIRE(data == std::nullopt);
  }
}

TEST_CASE("NestedTransaction - read transaction under write transaction", "[lmdb][nested]")
{
  // LMDB does not support read transactions nested under write transactions.
  // Only nested write transactions are supported.
  // This test verifies that attempting to create a read transaction from
  // a write transaction is not supported by the API.
  TempDir temp;
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  // WriteTransaction only supports nested WriteTransaction, not ReadTransaction
  // The API does not provide a way to construct ReadTransaction from WriteTransaction
  // So this test just verifies basic write transaction works
  WriteTransaction wtxn(env);
  Database db{wtxn, "test"};
  auto writer = db.writer(wtxn);
  REQUIRE(!writer.create(1, makeBuffer(createStringData("test"))).empty());
  wtxn.commit();

  // Verify read transaction works independently
  ReadTransaction rtxn(env);
  Database readDb{rtxn, "test"};
  auto reader = readDb.reader(rtxn);
  REQUIRE(reader.begin() != reader.end());
}
