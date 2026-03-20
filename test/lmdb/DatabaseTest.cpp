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
// Database Tests
// ============================================================================

TEST_CASE("Database - constructor opens database", "[lmdb][database]")
{
  TempDir temp;
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  WriteTransaction parentTxn(env);
  WriteTransaction dbTxn(parentTxn);  // Nested transaction
  Database db{dbTxn, "newdb"};
  dbTxn.commit();
  parentTxn.commit();
  // Should be able to start transaction
  ReadTransaction txn(env);
  auto reader = db.reader(txn);
  REQUIRE(reader.begin() == reader.end());
}

// ============================================================================
// Database::Reader Tests
// ============================================================================

TEST_CASE("Database::Reader - begin and end", "[lmdb][database][reader]")
{
  TempDir temp;
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  // Create and populate database
  {
    WriteTransaction parentTxn(env);
    WriteTransaction dbTxn(parentTxn);
    Database db{dbTxn, "test"};
    dbTxn.commit();

    WriteTransaction wtxn(parentTxn);
    auto writer = db.writer(wtxn);
    writer.create(1, makeBuffer(createStringData("one")));
    writer.create(2, makeBuffer(createStringData("two")));
    wtxn.commit();

    parentTxn.commit();
  }

  // Test reader iterator
  ReadTransaction rtxn(env);
  Database db{rtxn, "test"};
  auto reader = db.reader(rtxn);

  auto it = reader.begin();
  REQUIRE(it != reader.end());
  REQUIRE(it->first == 1);

  ++it;
  REQUIRE(it != reader.end());
  REQUIRE(it->first == 2);

  ++it;
  REQUIRE(it == reader.end());
}

TEST_CASE("Database::Reader - empty database", "[lmdb][database][reader]")
{
  TempDir temp;
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  WriteTransaction wtxn(env);
  Database db{wtxn, "test"};
  wtxn.commit();

  ReadTransaction rtxn(env);
  auto reader = db.reader(rtxn);
  REQUIRE(reader.begin() == reader.end());
}

TEST_CASE("Database::Reader - get", "[lmdb][database][reader]")
{
  TempDir temp;
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  WriteTransaction parentTxn(env);
  WriteTransaction dbTxn(parentTxn);
  Database db{dbTxn, "test"};
  dbTxn.commit();

  WriteTransaction wtxn(parentTxn);
  auto writer = db.writer(wtxn);
  writer.create(42, makeBuffer(createStringData("answer")));
  wtxn.commit();

  parentTxn.commit();

  ReadTransaction rtxn(env);
  auto reader = db.reader(rtxn);

  SECTION("Existing key returns data")
  {
    auto data = reader.get(42);
    REQUIRE(data.has_value());
    REQUIRE(data->size() == 6);
    REQUIRE(std::strncmp(reinterpret_cast<const char*>(data->data()), "answer", 6) == 0);
  }

  SECTION("Missing key returns nullopt")
  {
    auto data = reader.get(999);
    REQUIRE(data == std::nullopt);
  }
}

TEST_CASE("Database::Reader::Iterator - default constructor", "[lmdb][database][reader]")
{
  Database::Reader::Iterator it;
  // Default constructed iterator should equal end
}

TEST_CASE("Database::Reader::Iterator - copy constructor", "[lmdb][database][reader]")
{
  TempDir temp;
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  WriteTransaction parentTxn(env);
  WriteTransaction dbTxn(parentTxn);
  Database db{dbTxn, "test"};
  dbTxn.commit();

  WriteTransaction wtxn(parentTxn);
  db.writer(wtxn).create(1, makeBuffer(createStringData("data")));
  wtxn.commit();

  parentTxn.commit();

  ReadTransaction rtxn(env);
  auto reader = db.reader(rtxn);

  auto it1 = reader.begin();
  auto it2 = it1; // Copy

  REQUIRE(it1 == it2);
}

TEST_CASE("Database::Reader::Iterator - move constructor", "[lmdb][database][reader]")
{
  TempDir temp;
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  WriteTransaction parentTxn(env);
  WriteTransaction dbTxn(parentTxn);
  Database db{dbTxn, "test"};
  dbTxn.commit();

  WriteTransaction wtxn(parentTxn);
  db.writer(wtxn).create(1, makeBuffer(createStringData("data")));
  wtxn.commit();

  parentTxn.commit();

  ReadTransaction rtxn(env);
  auto reader = db.reader(rtxn);

  auto it1 = reader.begin();
  auto it2 = std::move(it1);
  // it2 should be valid
}

TEST_CASE("Database::Reader::Iterator - dereference", "[lmdb][database][reader]")
{
  TempDir temp;
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  WriteTransaction parentTxn(env);
  WriteTransaction dbTxn(parentTxn);
  Database db{dbTxn, "test"};
  dbTxn.commit();

  WriteTransaction wtxn(parentTxn);
  auto writer = db.writer(wtxn);
  writer.create(100, makeBuffer(createStringData("value")));
  wtxn.commit();

  parentTxn.commit();

  ReadTransaction rtxn(env);
  auto reader = db.reader(rtxn);

  auto it = reader.begin();
  auto& value = *it;

  REQUIRE(value.first == 100);
  REQUIRE(value.second.size() == 5);
}

// ============================================================================
// Database::Writer Tests
// ============================================================================

TEST_CASE("Database::Writer - create with id and data", "[lmdb][database][writer]")
{
  TempDir temp;
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  WriteTransaction wtxn(env);
  Database db{wtxn, "test"};
  auto writer = db.writer(wtxn);

  writer.create(1, makeBuffer(createStringData("hello")));

  wtxn.commit();

  // Verify via reader
  ReadTransaction rtxn(env);
  auto reader = db.reader(rtxn);
  auto data = reader.get(1);
  REQUIRE(data.has_value());
  REQUIRE(data->size() == 5);
}

TEST_CASE("Database::Writer - create with id and size", "[lmdb][database][writer]")
{
  TempDir temp;
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  WriteTransaction wtxn(env);
  Database db{wtxn, "test"};
  auto writer = db.writer(wtxn);

  auto result = writer.create(1, 10);
  REQUIRE(!result.empty());
  REQUIRE(result.size() == 10);

  // Write data into the reserved space
  std::memset(result.data(), 'x', 10);

  wtxn.commit();

  // Verify via reader
  ReadTransaction rtxn(env);
  auto reader = db.reader(rtxn);
  auto data = reader.get(1);
  REQUIRE(data.has_value());
  REQUIRE(data->size() == 10);
}

TEST_CASE("Database::Writer - append with data", "[lmdb][database][writer]")
{
  TempDir temp;
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  WriteTransaction wtxn(env);
  Database db{wtxn, "test"};
  auto writer = db.writer(wtxn);

  auto id1 = writer.append(makeBuffer(createStringData("first")));
  REQUIRE(id1 == 0);

  auto id2 = writer.append(makeBuffer(createStringData("second")));
  REQUIRE(id2 == 1);

  wtxn.commit();

  // Verify via reader
  ReadTransaction rtxn(env);
  auto reader = db.reader(rtxn);
  auto data1 = reader.get(0);
  auto data2 = reader.get(1);
  REQUIRE(data1.has_value());
  REQUIRE(data2.has_value());
}

TEST_CASE("Database::Writer - append with size", "[lmdb][database][writer]")
{
  TempDir temp;
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  WriteTransaction wtxn(env);
  Database db{wtxn, "test"};
  auto writer = db.writer(wtxn);

  auto [id1, result1] = writer.append(8);
  REQUIRE(id1 == 0);
  REQUIRE(!result1.empty());
  REQUIRE(result1.size() == 8);
  std::memset(result1.data(), 'a', 8);

  auto [id2, result2] = writer.append(12);
  REQUIRE(id2 == 1);
  REQUIRE(!result2.empty());
  REQUIRE(result2.size() == 12);
  std::memset(result2.data(), 'b', 12);

  wtxn.commit();

  // Verify via reader
  ReadTransaction rtxn(env);
  auto reader = db.reader(rtxn);
  auto data1 = reader.get(0);
  auto data2 = reader.get(1);
  REQUIRE(data1.has_value());
  REQUIRE(data2.has_value());
}

TEST_CASE("Database::Writer - update existing record", "[lmdb][database][writer]")
{
  TempDir temp;
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  WriteTransaction wtxn(env);
  Database db{wtxn, "test"};
  auto writer = db.writer(wtxn);

  // Create initial record
  {
    writer.create(1, makeBuffer(createStringData("original")));
  }
  wtxn.commit();

  // Update the record
  WriteTransaction wtxn2(env);
  Database db2{wtxn2, "test"};
  auto writer2 = db2.writer(wtxn2);
  auto result2 = writer2.update(1, makeBuffer(createStringData("updated")));
  REQUIRE(!result2.empty());
  wtxn2.commit();

  // Verify via reader
  ReadTransaction rtxn(env);
  Database verifyDb{rtxn, "test"};
  auto reader = verifyDb.reader(rtxn);
  auto data = reader.get(1);
  REQUIRE(data.has_value());
  REQUIRE(data->size() == 7);  // "updated" has 7 characters
}

TEST_CASE("Database::Writer - delete record", "[lmdb][database][writer]")
{
  TempDir temp;
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  WriteTransaction wtxn(env);
  Database db{wtxn, "test"};
  auto writer = db.writer(wtxn);
  writer.create(1, makeBuffer(createStringData("test")));
  wtxn.commit();

  // Verify exists
  {
    ReadTransaction rtxn(env);
    Database readDb{rtxn, "test"};
    auto reader = readDb.reader(rtxn);
    auto data1 = reader.get(1);
    REQUIRE(data1.has_value());
  }

  // Delete
  {
    WriteTransaction wtxn2(env);
    Database db2{wtxn2, "test"};
    auto writer2 = db2.writer(wtxn2);
    bool deleted = writer2.del(1);
    REQUIRE(deleted);
    wtxn2.commit();
  }

  // Verify deleted
  {
    ReadTransaction rtxn2(env);
    Database readDb2{rtxn2, "test"};
    auto reader2 = readDb2.reader(rtxn2);
    auto data2 = reader2.get(1);
    REQUIRE(!data2.has_value());
  }
}

TEST_CASE("Database::Writer - get within write transaction", "[lmdb][database][writer]")
{
  TempDir temp;
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  WriteTransaction wtxn(env);
  Database db{wtxn, "test"};
  auto writer = db.writer(wtxn);

  writer.create(42, makeBuffer(createStringData("answer")));
  auto data = writer.get(42);
  REQUIRE(data.has_value());
  REQUIRE(data->size() == 6);
}

TEST_CASE("Database::Writer - move constructor", "[lmdb][database][writer]")
{
  TempDir temp;
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  WriteTransaction wtxn(env);
  Database db{wtxn, "test"};

  auto writer1 = db.writer(wtxn);
  writer1.create(1, makeBuffer(createStringData("test")));

  Database::Writer writer2{std::move(writer1)};
  // writer1 is now in moved-from state
  // writer2 should still be usable

  wtxn.commit();
}

