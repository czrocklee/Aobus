// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <catch2/catch_test_macros.hpp>

#include <ao/Exception.h>
#include <ao/lmdb/Database.h>
#include <ao/lmdb/Environment.h>
#include <ao/lmdb/Transaction.h>
#include <ao/utility/ByteView.h>
#include <lmdb.h>
#include <test/unit/lmdb/TestUtils.h>

#include <cstring>
#include <optional>
#include <string_view>
#include <utility>

namespace ao::lmdb::test
{
  TEST_CASE("Database - constructor opens database", "[lmdb][database]")
  {
    auto const temp = TempDir{};
    auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

    auto wtxn = WriteTransaction{env};
    auto db = Database{wtxn, "newdb"};
    wtxn.commit();

    auto const txn = ReadTransaction{env};
    auto const reader = db.reader(txn);
    REQUIRE(reader.begin() == reader.end());
  }

  // ============================================================================
  // Database::Reader Tests
  // ============================================================================
  TEST_CASE("Database::Reader - begin and end", "[lmdb][database][reader]")
  {
    auto const temp = TempDir{};
    auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

    auto wtxn = WriteTransaction{env};
    auto db = Database{wtxn, "test"};
    auto writer = db.writer(wtxn);
    writer.create(1, createStringData("one"));
    writer.create(2, createStringData("two"));
    wtxn.commit();

    // Test reader iterator
    auto const rtxn = ReadTransaction{env};
    auto const reader = db.reader(rtxn);

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
    auto const temp = TempDir{};
    auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

    auto wtxn = WriteTransaction{env};
    auto db = Database{wtxn, "test"};
    wtxn.commit();

    auto const rtxn = ReadTransaction{env};
    auto const reader = db.reader(rtxn);
    REQUIRE(reader.begin() == reader.end());
  }

  TEST_CASE("Database::Reader - get", "[lmdb][database][reader]")
  {
    auto const temp = TempDir{};
    auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

    auto wtxn = WriteTransaction{env};
    auto db = Database{wtxn, "test"};
    auto writer = db.writer(wtxn);
    writer.create(42, createStringData("answer"));
    wtxn.commit();

    auto const rtxn = ReadTransaction{env};
    auto const reader = db.reader(rtxn);

    SECTION("Existing key returns data")
    {
      auto const optData = reader.get(42);
      REQUIRE(optData.has_value());
      REQUIRE(utility::bytes::stringView(*optData) == "answer");
    }

    SECTION("Missing key returns nullopt")
    {
      auto const optData = reader.get(999);
      REQUIRE(optData == std::nullopt);
    }
  }

  TEST_CASE("Database::Reader::Iterator - default constructor", "[lmdb][database][reader]")
  {
    auto const it = Database::Reader::Iterator{};
    // Default constructed iterator should equal end
  }

  TEST_CASE("Database::Reader::Iterator - copy constructor", "[lmdb][database][reader]")
  {
    auto const temp = TempDir{};
    auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

    auto wtxn = WriteTransaction{env};
    auto db = Database{wtxn, "test"};
    db.writer(wtxn).create(1, createStringData("data"));
    wtxn.commit();

    auto const rtxn = ReadTransaction{env};
    auto const reader = db.reader(rtxn);

    auto const it1 = reader.begin();
    auto const& it2 = it1; // Copy

    REQUIRE(it1 == it2);
  }

  TEST_CASE("Database::Reader::Iterator - move constructor", "[lmdb][database][reader]")
  {
    auto const temp = TempDir{};
    auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

    auto wtxn = WriteTransaction{env};
    auto db = Database{wtxn, "test"};
    db.writer(wtxn).create(1, createStringData("data"));
    wtxn.commit();

    auto const rtxn = ReadTransaction{env};
    auto const reader = db.reader(rtxn);

    auto it1 = reader.begin();
    auto const it2 = Database::Reader::Iterator{std::move(it1)};
    // it2 should be valid
  }

  TEST_CASE("Database::Reader::Iterator - dereference", "[lmdb][database][reader]")
  {
    auto const temp = TempDir{};
    auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

    auto wtxn = WriteTransaction{env};
    auto db = Database{wtxn, "test"};
    auto writer = db.writer(wtxn);
    writer.create(100, createStringData("value"));
    wtxn.commit();

    auto const rtxn = ReadTransaction{env};
    auto const reader = db.reader(rtxn);
    auto const it = reader.begin();
    REQUIRE(utility::bytes::stringView(it->second) == "value");
  }

  // ============================================================================
  // Database::Writer Tests
  // ============================================================================
  TEST_CASE("Database::Writer - create with id and data", "[lmdb][database][writer]")
  {
    auto const temp = TempDir{};
    auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

    auto wtxn = WriteTransaction{env};
    auto db = Database{wtxn, "test"};
    auto writer = db.writer(wtxn);
    writer.create(1, createStringData("hello"));
    wtxn.commit();

    // Verify via reader
    auto const rtxn = ReadTransaction{env};
    auto const reader = db.reader(rtxn);
    auto const optData = reader.get(1);
    REQUIRE(optData.has_value());
    REQUIRE(utility::bytes::stringView(*optData) == "hello");
  }

  TEST_CASE("Database::Writer - create with id and size", "[lmdb][database][writer]")
  {
    auto const temp = TempDir{};
    auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

    auto wtxn = WriteTransaction{env};
    auto db = Database{wtxn, "test"};
    auto writer = db.writer(wtxn);
    auto const result = writer.create(1, 10);
    REQUIRE(!result.empty());
    REQUIRE(result.size() == 10);

    // Write data into the reserved space
    std::memset(result.data(), 'x', 10);

    wtxn.commit();

    // Verify via reader
    auto const rtxn = ReadTransaction{env};
    auto const reader = db.reader(rtxn);
    auto const optData = reader.get(1);
    REQUIRE(optData.has_value());
    REQUIRE(optData->size() == 10);
    REQUIRE(utility::bytes::stringView(*optData) == std::string(10, 'x'));
  }

  TEST_CASE("Database::Writer - append with data", "[lmdb][database][writer]")
  {
    auto const temp = TempDir{};
    auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

    auto wtxn = WriteTransaction{env};
    auto db = Database{wtxn, "test"};
    auto writer = db.writer(wtxn);

    auto const id1 = writer.append(createStringData("first"));
    REQUIRE(id1 == 1);

    auto const id2 = writer.append(createStringData("second"));
    REQUIRE(id2 == 2);

    wtxn.commit();

    // Verify via reader
    auto const rtxn = ReadTransaction{env};
    auto const reader = db.reader(rtxn);
    auto const optData1 = reader.get(1);
    auto const optData2 = reader.get(2);
    REQUIRE(optData1.has_value());
    REQUIRE(optData2.has_value());
    REQUIRE(utility::bytes::stringView(*optData1) == "first");
    REQUIRE(utility::bytes::stringView(*optData2) == "second");
  }

  TEST_CASE("Database::Writer - append with size", "[lmdb][database][writer]")
  {
    auto const temp = TempDir{};
    auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

    auto wtxn = WriteTransaction{env};
    auto db = Database{wtxn, "test"};
    auto writer = db.writer(wtxn);

    auto const [id1, result1] = writer.append(8);
    REQUIRE(id1 == 1);
    REQUIRE(!result1.empty());
    REQUIRE(result1.size() == 8);
    std::memset(result1.data(), 'a', 8);

    auto const [id2, result2] = writer.append(12);
    REQUIRE(id2 == 2);
    REQUIRE(!result2.empty());
    REQUIRE(result2.size() == 12);
    std::memset(result2.data(), 'b', 12);

    wtxn.commit();

    // Verify via reader
    auto const rtxn = ReadTransaction{env};
    auto const reader = db.reader(rtxn);
    auto const optData1 = reader.get(1);
    auto const optData2 = reader.get(2);
    REQUIRE(optData1.has_value());
    REQUIRE(optData2.has_value());
    REQUIRE(utility::bytes::stringView(*optData1) == std::string(8, 'a'));
    REQUIRE(utility::bytes::stringView(*optData2) == std::string(12, 'b'));
  }

  TEST_CASE("Database::Writer - update existing record", "[lmdb][database][writer]")
  {
    auto const temp = TempDir{};
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
    auto const rtxn = ReadTransaction{env};
    auto const reader = db.reader(rtxn);
    auto const optData = reader.get(1);
    REQUIRE(optData.has_value());
    REQUIRE(optData->size() == 7);
    REQUIRE(utility::bytes::stringView(*optData) == "updated");
  }

  TEST_CASE("Database::Writer - delete record", "[lmdb][database][writer]")
  {
    auto const temp = TempDir{};
    auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

    auto wtxn = WriteTransaction{env};
    auto db = Database{wtxn, "test"};
    auto writer = db.writer(wtxn);
    writer.create(1, createStringData("test"));
    wtxn.commit();

    // Verify exists
    {
      auto const rtxn = ReadTransaction{env};
      auto const reader = db.reader(rtxn);
      auto const optData1 = reader.get(1);
      REQUIRE(optData1.has_value());
    }

    // Delete
    {
      auto wtxn = WriteTransaction{env};
      auto writer = db.writer(wtxn);
      bool const deleted = writer.del(1);
      REQUIRE(deleted);
      wtxn.commit();
    }

    // Verify deleted
    {
      auto const rtxn = ReadTransaction{env};
      auto const reader = db.reader(rtxn);
      auto const optData2 = reader.get(1);
      REQUIRE(!optData2.has_value());
    }
  }

  TEST_CASE("Database::Writer - get within write transaction", "[lmdb][database][writer]")
  {
    auto const temp = TempDir{};
    auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

    auto wtxn = WriteTransaction{env};
    auto db = Database{wtxn, "test"};
    auto writer = db.writer(wtxn);

    writer.create(42, createStringData("answer"));
    auto const optData = writer.get(42);
    REQUIRE(optData.has_value());
    REQUIRE(optData->size() == 6);
    REQUIRE(utility::bytes::stringView(*optData) == "answer");
  }

  TEST_CASE("Database::Writer - move constructor", "[lmdb][database][writer]")
  {
    auto const temp = TempDir{};
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
    auto const temp = TempDir{};
    auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

    auto wtxn = WriteTransaction{env};
    auto db = Database{wtxn, "test"};
    auto writer = db.writer(wtxn);

    writer.create(1, createStringData("first"));

    REQUIRE_THROWS_AS(writer.create(1, createStringData("duplicate")), Exception);
    wtxn.commit();
  }

  TEST_CASE("Database::Writer - create throws on duplicate id with size", "[lmdb][database][writer]")
  {
    auto const temp = TempDir{};
    auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

    auto wtxn = WriteTransaction{env};
    auto db = Database{wtxn, "test"};
    auto writer = db.writer(wtxn);

    writer.create(1, 10);

    REQUIRE_THROWS_AS(writer.create(1, 5), Exception);
    wtxn.commit();
  }

  TEST_CASE("Database::Reader - maxKey on empty database", "[lmdb][database][reader]")
  {
    auto const temp = TempDir{};
    auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

    auto wtxn = WriteTransaction{env};
    auto db = Database{wtxn, "test"};
    wtxn.commit();

    auto const rtxn = ReadTransaction{env};
    auto const reader = db.reader(rtxn);
    REQUIRE(reader.maxKey() == 0);
  }

  TEST_CASE("Database::Reader - maxKey after append", "[lmdb][database][reader]")
  {
    auto const temp = TempDir{};
    auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

    auto wtxn = WriteTransaction{env};
    auto db = Database{wtxn, "test"};
    auto writer = db.writer(wtxn);

    writer.append(createStringData("first"));
    auto const id2 = writer.append(createStringData("second"));
    wtxn.commit();

    auto const rtxn = ReadTransaction{env};
    auto const reader = db.reader(rtxn);
    REQUIRE(reader.maxKey() == id2);
  }

  TEST_CASE("Database::Reader - maxKey after create", "[lmdb][database][reader]")
  {
    auto const temp = TempDir{};
    auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

    auto wtxn = WriteTransaction{env};
    auto db = Database{wtxn, "test"};
    auto writer = db.writer(wtxn);

    writer.create(5, createStringData("five"));
    writer.create(10, createStringData("ten"));
    writer.create(3, createStringData("three"));
    wtxn.commit();

    auto const rtxn = ReadTransaction{env};
    auto const reader = db.reader(rtxn);
    REQUIRE(reader.maxKey() == 10);
  }

  TEST_CASE("Database::Reader - maxKey after delete", "[lmdb][database][reader]")
  {
    auto const temp = TempDir{};
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

    auto const rtxn = ReadTransaction{env};
    auto const reader = db.reader(rtxn);
    REQUIRE(reader.maxKey() == 2);
  }

  TEST_CASE("Database::Reader - maxKey after deleting middle element", "[lmdb][database][reader]")
  {
    auto const temp = TempDir{};
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

    auto const rtxn = ReadTransaction{env};
    auto const reader = db.reader(rtxn);
    // maxKey should still be 3 (the max wasn't deleted)
    REQUIRE(reader.maxKey() == 3);
  }
} // namespace ao::lmdb::test
