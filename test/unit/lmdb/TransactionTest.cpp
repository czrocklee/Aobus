// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators_all.hpp>
#include <catch2/matchers/catch_matchers_all.hpp>

#include <lmdb.h>
#include <rs/lmdb/Database.h>
#include <rs/lmdb/Environment.h>
#include <rs/lmdb/Transaction.h>
#include <test/unit/lmdb/TestUtils.h>

#include <cstring>
#include <optional>
#include <string_view>

using namespace rs::lmdb;

// ============================================================================
// ReadTransaction Tests
// ============================================================================

TEST_CASE("ReadTransaction - constructor starts transaction", "[lmdb][transaction]")
{
  auto temp = TempDir{};
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  // First create database with write transaction
  auto wtxn = WriteTransaction{env};
  auto db = Database{wtxn, "test"};
  wtxn.commit();

  // Then use read transaction
  auto txn = ReadTransaction{env};
  auto reader = db.reader(txn);
  REQUIRE(reader.begin() == reader.end()); // Empty DB
}

TEST_CASE("ReadTransaction - destructor aborts", "[lmdb][transaction]")
{
  auto temp = TempDir{};
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  // Create database with write transaction
  auto wtxn = WriteTransaction{env};
  auto db = Database{wtxn, "test"};
  wtxn.commit();

  // Read transaction - destructor should abort
  {
    auto txn = ReadTransaction{env};
    (void)db;
    // Destructor should abort
  }
  // Should be able to start new transaction
  auto txn2 = ReadTransaction{env};
  auto reader = db.reader(txn2);
  REQUIRE(reader.begin() == reader.end());
}

TEST_CASE("ReadTransaction - move", "[lmdb][transaction]")
{
  auto temp = TempDir{};
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  // Create database first with write transaction
  auto wtxn = WriteTransaction{env};
  auto db = Database{wtxn, "test"};
  wtxn.commit();

  auto txn1 = ReadTransaction{env};
  auto txn2 = ReadTransaction{std::move(txn1)};
  // Verify moved transaction is valid by using it
  auto reader = db.reader(txn2);
  REQUIRE(reader.begin() == reader.end());
}

// ============================================================================
// WriteTransaction Tests
// ============================================================================

TEST_CASE("WriteTransaction - constructor starts transaction", "[lmdb][transaction]")
{
  auto temp = TempDir{};
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  auto txn = WriteTransaction{env};
  // Verify transaction is valid by using it to create a database
  auto db = Database{txn, "test"};
  auto writer = db.writer(txn);
  (void)writer;
}

TEST_CASE("WriteTransaction - commit", "[lmdb][transaction]")
{
  auto temp = TempDir{};
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  // Create database, write data, commit
  auto wtxn = WriteTransaction{env};
  auto db = Database{wtxn, "test"};
  auto writer = db.writer(wtxn);
  writer.create(1, createStringData("test data"));
  wtxn.commit();

  // Start a new transaction - should work now
  auto wtxn2 = WriteTransaction{env};
  auto reader = db.reader(wtxn2);
  auto it = reader.begin();
  REQUIRE(it != reader.end());
  REQUIRE(it->first == 1);
}

TEST_CASE("WriteTransaction - destructor without commit aborts", "[lmdb][transaction]")
{
  auto temp = TempDir{};
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  // Create database
  auto dbTxn = WriteTransaction{env};
  auto db = Database{dbTxn, "test"};
  dbTxn.commit();

  // Write data without committing - transaction should abort on destruction
  {
    auto txn = WriteTransaction{env};
    auto writer = db.writer(txn);
    writer.create(1, createStringData("uncommitted"));
    // Without commit, transaction aborts on destruction
  }

  // Data should not be visible
  auto txn = ReadTransaction{env};
  auto reader = db.reader(txn);
  auto data = reader.get(1);
  REQUIRE(data == std::nullopt);
}

TEST_CASE("WriteTransaction - move", "[lmdb][transaction]")
{
  auto temp = TempDir{};
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  // First create the database
  auto wtxn = WriteTransaction{env};
  auto db = Database{wtxn, "test"};
  wtxn.commit();

  // Now test move
  auto txn1 = WriteTransaction{env};
  auto txn2 = WriteTransaction{std::move(txn1)};
  // Verify moved transaction is valid by using it
  [[maybe_unused]] auto writer = db.writer(txn2);
  txn2.commit();
}

// ============================================================================
// Nested Transaction Tests
// ============================================================================

TEST_CASE("NestedTransaction - child commit merges to parent", "[lmdb][nested]")
{
  auto temp = TempDir{};
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  // Create parent write transaction and open database
  auto parentTxn = WriteTransaction{env};
  auto db = Database{parentTxn, "test"};

  // Create nested write transaction
  auto childTxn = WriteTransaction{parentTxn};
  auto writer = db.writer(childTxn);
  writer.create(1, createStringData("nested data"));

  // Commit child - merges to parent
  childTxn.commit();
  // Parent should still be valid
  parentTxn.commit();

  // Verify data is visible
  auto rtxn = ReadTransaction{env};
  auto reader = db.reader(rtxn);
  auto it = reader.begin();
  REQUIRE(it != reader.end());
  REQUIRE(it->first == 1);
}

TEST_CASE("NestedTransaction - child abort does not affect parent", "[lmdb][nested]")
{
  auto temp = TempDir{};
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  // Create parent transaction and open database
  auto parentTxn = WriteTransaction{env};
  auto db = Database{parentTxn, "test"};

  // Create child transaction and write data
  {
    auto childTxn = WriteTransaction{parentTxn};
    auto writer = db.writer(childTxn);
    writer.create(1, createStringData("child data"));
    // Child transaction aborts on destruction without commit
  }

  // Parent commits - child data should NOT be included
  parentTxn.commit();

  // Verify data is NOT visible
  auto rtxn = ReadTransaction{env};
  auto reader = db.reader(rtxn);
  auto data = reader.get(1);
  REQUIRE(data == std::nullopt);
}