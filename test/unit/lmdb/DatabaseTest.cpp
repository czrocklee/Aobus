// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_all.hpp>
#include <catch2/generators/catch_generators_all.hpp>
#include <catch2/catch_approx.hpp>

#include <lmdb.h>
#include <rs/Exception.h>
#include <rs/lmdb/Database.h>
#include <rs/lmdb/Environment.h>
#include <rs/lmdb/Transaction.h>
#include <test/unit/lmdb/TestUtils.h>

#include <cstring>
#include <optional>
#include <string_view>

#include <rs/utility/ByteView.h>

using namespace rs::lmdb;

// ============================================================================
// Database Tests
// ============================================================================

TEST_CASE("Database - constructor opens database", "[lmdb][database]")
{
  auto temp = TempDir{};
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  auto wtxn = WriteTransaction{env};
  auto db = Database{wtxn, "newdb"};
  wtxn.commit();

  auto txn = ReadTransaction{env};
  auto reader = db.reader(txn);
  REQUIRE(reader.begin() == reader.end());
}

// ============================================================================
// Database::Reader Tests
// ============================================================================

TEST_CASE("Database::Reader - begin and end", "[lmdb][database][reader]")
{
  auto temp = TempDir{};
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  auto wtxn = WriteTransaction{env};
  auto db = Database{wtxn, "test"};
  auto writer = db.writer(wtxn);
  writer.create(1, createStringData("one"));
  writer.create(2, createStringData("two"));
  wtxn.commit();

  // Test reader iterator
  auto rtxn = ReadTransaction{env};
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
  auto temp = TempDir{};
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  auto wtxn = WriteTransaction{env};
  auto db = Database{wtxn, "test"};
  wtxn.commit();

  auto rtxn = ReadTransaction{env};
  auto reader = db.reader(rtxn);
  REQUIRE(reader.begin() == reader.end());
}

TEST_CASE("Database::Reader - get", "[lmdb][database][reader]")
{
  auto temp = TempDir{};
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  auto wtxn = WriteTransaction{env};
  auto db = Database{wtxn, "test"};
  auto writer = db.writer(wtxn);
  writer.create(42, createStringData("answer"));
  wtxn.commit();

  auto rtxn = ReadTransaction{env};
  auto reader = db.reader(rtxn);

  SECTION("Existing key returns data")
  {
    auto data = reader.get(42);
    REQUIRE(data.has_value());
    REQUIRE(rs::utility::bytes::stringView(*data) == "answer");
  }

  SECTION("Missing key returns nullopt")
  {
    auto data = reader.get(999);
    REQUIRE(data == std::nullopt);
  }
}

TEST_CASE("Database::Reader::Iterator - default constructor", "[lmdb][database][reader]")
{
  auto it = Database::Reader::Iterator{};
  // Default constructed iterator should equal end
}

TEST_CASE("Database::Reader::Iterator - copy constructor", "[lmdb][database][reader]")
{
  auto temp = TempDir{};
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  auto wtxn = WriteTransaction{env};
  auto db = Database{wtxn, "test"};
  db.writer(wtxn).create(1, createStringData("data"));
  wtxn.commit();

  auto rtxn = ReadTransaction{env};
  auto reader = db.reader(rtxn);

  auto it1 = reader.begin();
  auto it2 = it1; // Copy

  REQUIRE(it1 == it2);
}

TEST_CASE("Database::Reader::Iterator - move constructor", "[lmdb][database][reader]")
{
  auto temp = TempDir{};
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  auto wtxn = WriteTransaction{env};
  auto db = Database{wtxn, "test"};
  db.writer(wtxn).create(1, createStringData("data"));
  wtxn.commit();

  auto rtxn = ReadTransaction{env};
  auto reader = db.reader(rtxn);

  auto it1 = reader.begin();
  auto it2 = std::move(it1);
  // it2 should be valid
}

TEST_CASE("Database::Reader::Iterator - dereference", "[lmdb][database][reader]")
{
  auto temp = TempDir{};
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  auto wtxn = WriteTransaction{env};
  auto db = Database{wtxn, "test"};
  auto writer = db.writer(wtxn);
  writer.create(100, createStringData("value"));
  wtxn.commit();

  auto rtxn = ReadTransaction{env};
  auto reader = db.reader(rtxn);
  auto it = reader.begin();
  REQUIRE(rs::utility::bytes::stringView(it->second) == "value");
}

// ============================================================================
// Database::Writer Tests
// ============================================================================

TEST_CASE("Database::Writer - create with id and data", "[lmdb][database][writer]")
{
  auto temp = TempDir{};
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  auto wtxn = WriteTransaction{env};
  auto db = Database{wtxn, "test"};
  auto writer = db.writer(wtxn);
  writer.create(1, createStringData("hello"));
  wtxn.commit();

  // Verify via reader
  auto rtxn = ReadTransaction{env};
  auto reader = db.reader(rtxn);
  auto data = reader.get(1);
  REQUIRE(data.has_value());
  REQUIRE(rs::utility::bytes::stringView(*data) == "hello");
}

TEST_CASE("Database::Writer - create with id and size", "[lmdb][database][writer]")
{
  auto temp = TempDir{};
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  auto wtxn = WriteTransaction{env};
  auto db = Database{wtxn, "test"};
  auto writer = db.writer(wtxn);
  auto result = writer.create(1, 10);
  REQUIRE(!result.empty());
  REQUIRE(result.size() == 10);

  // Write data into the reserved space
  std::memset(result.data(), 'x', 10);

  wtxn.commit();

  // Verify via reader
  auto rtxn = ReadTransaction{env};
  auto reader = db.reader(rtxn);
  auto data = reader.get(1);
  REQUIRE(data.has_value());
  REQUIRE(data->size() == 10);
  REQUIRE(rs::utility::bytes::stringView(*data) == std::string(10, 'x'));
}

TEST_CASE("Database::Writer - append with data", "[lmdb][database][writer]")
{
  auto temp = TempDir{};
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  auto wtxn = WriteTransaction{env};
  auto db = Database{wtxn, "test"};
  auto writer = db.writer(wtxn);

  auto id1 = writer.append(createStringData("first"));
  REQUIRE(id1 == 1);

  auto id2 = writer.append(createStringData("second"));
  REQUIRE(id2 == 2);

  wtxn.commit();

  // Verify via reader
  auto rtxn = ReadTransaction{env};
  auto reader = db.reader(rtxn);
  auto data1 = reader.get(1);
  auto data2 = reader.get(2);
  REQUIRE(data1.has_value());
  REQUIRE(data2.has_value());
  REQUIRE(rs::utility::bytes::stringView(*data1) == "first");
  REQUIRE(rs::utility::bytes::stringView(*data2) == "second");
}

TEST_CASE("Database::Writer - append with size", "[lmdb][database][writer]")
{
  auto temp = TempDir{};
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  auto wtxn = WriteTransaction{env};
  auto db = Database{wtxn, "test"};
  auto writer = db.writer(wtxn);

  auto [id1, result1] = writer.append(8);
  REQUIRE(id1 == 1);
  REQUIRE(!result1.empty());
  REQUIRE(result1.size() == 8);
  std::memset(result1.data(), 'a', 8);

  auto [id2, result2] = writer.append(12);
  REQUIRE(id2 == 2);
  REQUIRE(!result2.empty());
  REQUIRE(result2.size() == 12);
  std::memset(result2.data(), 'b', 12);

  wtxn.commit();

  // Verify via reader
  auto rtxn = ReadTransaction{env};
  auto reader = db.reader(rtxn);
  auto data1 = reader.get(1);
  auto data2 = reader.get(2);
  REQUIRE(data1.has_value());
  REQUIRE(data2.has_value());
  REQUIRE(rs::utility::bytes::stringView(*data1) == std::string(8, 'a'));
  REQUIRE(rs::utility::bytes::stringView(*data2) == std::string(12, 'b'));
}

TEST_CASE("Database::Writer - update existing record", "[lmdb][database][writer]")
{
  auto temp = TempDir{};
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  auto wtxn = WriteTransaction{env};
  auto db = Database{wtxn, "test"};
  auto writer = db.writer(wtxn);

  // Create initial record
  writer.create(1, createStringData("original"));
  wtxn.commit();

  // Update the record
  auto wtxn2 = WriteTransaction{env};
  auto writer2 = db.writer(wtxn2);
  writer2.update(1, createStringData("updated"));
  wtxn2.commit();

  // Verify via reader
  auto rtxn = ReadTransaction{env};
  auto reader = db.reader(rtxn);
  auto data = reader.get(1);
  REQUIRE(data.has_value());
  REQUIRE(data->size() == 7);
  REQUIRE(rs::utility::bytes::stringView(*data) == "updated");
}

TEST_CASE("Database::Writer - delete record", "[lmdb][database][writer]")
{
  auto temp = TempDir{};
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  auto wtxn = WriteTransaction{env};
  auto db = Database{wtxn, "test"};
  auto writer = db.writer(wtxn);
  writer.create(1, createStringData("test"));
  wtxn.commit();

  // Verify exists
  {
    auto rtxn = ReadTransaction{env};
    auto reader = db.reader(rtxn);
    auto data1 = reader.get(1);
    REQUIRE(data1.has_value());
  }

  // Delete
  {
    auto wtxn = WriteTransaction{env};
    auto writer = db.writer(wtxn);
    bool deleted = writer.del(1);
    REQUIRE(deleted);
    wtxn.commit();
  }

  // Verify deleted
  {
    auto rtxn = ReadTransaction{env};
    auto reader = db.reader(rtxn);
    auto data2 = reader.get(1);
    REQUIRE(!data2.has_value());
  }
}

TEST_CASE("Database::Writer - get within write transaction", "[lmdb][database][writer]")
{
  auto temp = TempDir{};
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  auto wtxn = WriteTransaction{env};
  auto db = Database{wtxn, "test"};
  auto writer = db.writer(wtxn);

  writer.create(42, createStringData("answer"));
  auto data = writer.get(42);
  REQUIRE(data.has_value());
  REQUIRE(data->size() == 6);
  REQUIRE(rs::utility::bytes::stringView(*data) == "answer");
}

TEST_CASE("Database::Writer - move constructor", "[lmdb][database][writer]")
{
  auto temp = TempDir{};
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  auto wtxn = WriteTransaction{env};
  auto db = Database{wtxn, "test"};

  auto writer1 = db.writer(wtxn);
  writer1.create(1, createStringData("test"));

  auto writer2 = Database::Writer{std::move(writer1)};
  // writer1 is now in moved-from state
  // writer2 should still be usable

  wtxn.commit();
}

TEST_CASE("Database::Writer - create throws on duplicate id with data", "[lmdb][database][writer]")
{
  auto temp = TempDir{};
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  auto wtxn = WriteTransaction{env};
  auto db = Database{wtxn, "test"};
  auto writer = db.writer(wtxn);

  writer.create(1, createStringData("first"));

  REQUIRE_THROWS_AS(writer.create(1, createStringData("duplicate")), rs::Exception);
  wtxn.commit();
}

TEST_CASE("Database::Writer - create throws on duplicate id with size", "[lmdb][database][writer]")
{
  auto temp = TempDir{};
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  auto wtxn = WriteTransaction{env};
  auto db = Database{wtxn, "test"};
  auto writer = db.writer(wtxn);

  writer.create(1, 10);

  REQUIRE_THROWS_AS(writer.create(1, 5), rs::Exception);
  wtxn.commit();
}

TEST_CASE("Database::Reader - maxKey on empty database", "[lmdb][database][reader]")
{
  auto temp = TempDir{};
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  auto wtxn = WriteTransaction{env};
  auto db = Database{wtxn, "test"};
  wtxn.commit();

  auto rtxn = ReadTransaction{env};
  auto reader = db.reader(rtxn);
  REQUIRE(reader.maxKey() == 0);
}

TEST_CASE("Database::Reader - maxKey after append", "[lmdb][database][reader]")
{
  auto temp = TempDir{};
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  auto wtxn = WriteTransaction{env};
  auto db = Database{wtxn, "test"};
  auto writer = db.writer(wtxn);

  writer.append(createStringData("first"));
  auto id2 = writer.append(createStringData("second"));
  wtxn.commit();

  auto rtxn = ReadTransaction{env};
  auto reader = db.reader(rtxn);
  REQUIRE(reader.maxKey() == id2);
}

TEST_CASE("Database::Reader - maxKey after create", "[lmdb][database][reader]")
{
  auto temp = TempDir{};
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  auto wtxn = WriteTransaction{env};
  auto db = Database{wtxn, "test"};
  auto writer = db.writer(wtxn);

  writer.create(5, createStringData("five"));
  writer.create(10, createStringData("ten"));
  writer.create(3, createStringData("three"));
  wtxn.commit();

  auto rtxn = ReadTransaction{env};
  auto reader = db.reader(rtxn);
  REQUIRE(reader.maxKey() == 10);
}

TEST_CASE("Database::Reader - maxKey after delete", "[lmdb][database][reader]")
{
  auto temp = TempDir{};
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  auto wtxn = WriteTransaction{env};
  auto db = Database{wtxn, "test"};
  auto writer = db.writer(wtxn);

  writer.create(1, createStringData("one"));
  writer.create(2, createStringData("two"));
  writer.create(3, createStringData("three"));
  wtxn.commit();

  // Delete the max key
  {
    auto wtxn2 = WriteTransaction{env};
    auto writer2 = db.writer(wtxn2);
    writer2.del(3);
    wtxn2.commit();
  }

  auto rtxn = ReadTransaction{env};
  auto reader = db.reader(rtxn);
  REQUIRE(reader.maxKey() == 2);
}

TEST_CASE("Database::Reader - maxKey after deleting middle element", "[lmdb][database][reader]")
{
  auto temp = TempDir{};
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  auto wtxn = WriteTransaction{env};
  auto db = Database{wtxn, "test"};
  auto writer = db.writer(wtxn);

  writer.create(1, createStringData("one"));
  writer.create(2, createStringData("two"));
  writer.create(3, createStringData("three"));
  wtxn.commit();

  // Delete the middle element
  {
    auto wtxn2 = WriteTransaction{env};
    auto writer2 = db.writer(wtxn2);
    writer2.del(2);
    wtxn2.commit();
  }

  auto rtxn = ReadTransaction{env};
  auto reader = db.reader(rtxn);
  // maxKey should still be 3 (the max wasn't deleted)
  REQUIRE(reader.maxKey() == 3);
}
