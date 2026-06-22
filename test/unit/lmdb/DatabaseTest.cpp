// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "test/unit/lmdb/TestUtils.h"
#include <ao/Exception.h>
#include <ao/lmdb/Database.h>
#include <ao/lmdb/Environment.h>
#include <ao/lmdb/Transaction.h>
#include <ao/utility/ByteView.h>

#include <catch2/catch_test_macros.hpp>
#include <lmdb.h>

#include <cstdint>
#include <cstring>
#include <string_view>
#include <utility>

namespace ao::lmdb::test
{
  TEST_CASE("Database - helper opens database", "[lmdb][unit][database]")
  {
    auto const temp = TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto db = openDatabase(wtxn, "newdb");
    REQUIRE(wtxn.commit());

    auto const txn = beginReadTransaction(env);
    auto const reader = db.reader(txn);
    REQUIRE(reader.begin() == reader.end());
  }

  TEST_CASE("Database - open returns database", "[lmdb][unit][database]")
  {
    auto const temp = TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});
    auto wtxn = beginWriteTransaction(env);

    auto db = Database::open(wtxn, "newdb");

    REQUIRE(db);
    REQUIRE(wtxn.commit());
  }

  TEST_CASE("Database - read-only open returns NotFound for missing database", "[lmdb][unit][database]")
  {
    auto const temp = TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});
    auto txn = beginReadTransaction(env);

    auto db = Database::open(txn, "missing");

    REQUIRE_FALSE(db);
    CHECK(db.error().code == Error::Code::NotFound);
  }

  // ============================================================================
  // Database::Reader Tests
  // ============================================================================
  TEST_CASE("Database::Reader::Iterator - reaches end as normal state", "[lmdb][unit][database][reader]")
  {
    auto const temp = TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto db = openDatabase(wtxn, "test");
    auto writer = db.writer(wtxn);
    REQUIRE(writer.create(1, createStringData("one")));
    REQUIRE(writer.create(2, createStringData("two")));
    REQUIRE(wtxn.commit());

    // Test reader iterator
    auto const rtxn = beginReadTransaction(env);
    auto const reader = db.reader(rtxn);

    auto it = reader.begin();
    REQUIRE(it != reader.end());
    REQUIRE(it->first == 1);

    REQUIRE_NOTHROW(++it);
    REQUIRE(it != reader.end());
    REQUIRE(it->first == 2);

    REQUIRE_NOTHROW(++it);
    REQUIRE(it == reader.end());
  }

  TEST_CASE("Database::Reader - empty database", "[lmdb][unit][database][reader]")
  {
    auto const temp = TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto db = openDatabase(wtxn, "test");
    REQUIRE(wtxn.commit());

    auto const rtxn = beginReadTransaction(env);
    auto const reader = db.reader(rtxn);
    REQUIRE(reader.begin() == reader.end());
  }

  TEST_CASE("Database::Reader - get", "[lmdb][unit][database][reader]")
  {
    auto const temp = TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto db = openDatabase(wtxn, "test");
    auto writer = db.writer(wtxn);
    REQUIRE(writer.create(42, createStringData("answer")));
    REQUIRE(wtxn.commit());

    auto const rtxn = beginReadTransaction(env);
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
      REQUIRE_FALSE(optData.has_value());
    }
  }

  TEST_CASE("Database::Reader::Iterator - default constructor", "[lmdb][unit][database][reader]")
  {
    auto const it = Database::Reader::Iterator{};
    // Default constructed iterator should equal end
  }

  TEST_CASE("Database::Reader::Iterator - copy constructor", "[lmdb][unit][database][reader]")
  {
    auto const temp = TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto db = openDatabase(wtxn, "test");
    REQUIRE(db.writer(wtxn).create(1, createStringData("data")));
    REQUIRE(wtxn.commit());

    auto const rtxn = beginReadTransaction(env);
    auto const reader = db.reader(rtxn);

    auto const it1 = reader.begin();
    auto const& it2 = it1; // Copy

    REQUIRE(it1 == it2);
  }

  TEST_CASE("Database::Reader::Iterator - move constructor", "[lmdb][unit][database][reader]")
  {
    auto const temp = TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto db = openDatabase(wtxn, "test");
    REQUIRE(db.writer(wtxn).create(1, createStringData("data")));
    REQUIRE(wtxn.commit());

    auto const rtxn = beginReadTransaction(env);
    auto const reader = db.reader(rtxn);

    auto it1 = reader.begin();
    auto const it2 = Database::Reader::Iterator{std::move(it1)};
    // it2 should be valid
  }

  TEST_CASE("Database::Reader::Iterator - dereference", "[lmdb][unit][database][reader]")
  {
    auto const temp = TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto db = openDatabase(wtxn, "test");
    auto writer = db.writer(wtxn);
    REQUIRE(writer.create(100, createStringData("value")));
    REQUIRE(wtxn.commit());

    auto const rtxn = beginReadTransaction(env);
    auto const reader = db.reader(rtxn);
    auto const it = reader.begin();
    REQUIRE(utility::bytes::stringView(it->second) == "value");
  }

  // ============================================================================
  // Database::Writer Tests
  // ============================================================================
  TEST_CASE("Database::Writer - create with id and data", "[lmdb][unit][database][writer]")
  {
    auto const temp = TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto db = openDatabase(wtxn, "test");
    auto writer = db.writer(wtxn);
    REQUIRE(writer.create(1, createStringData("hello")));
    REQUIRE(wtxn.commit());

    // Verify via reader
    auto const rtxn = beginReadTransaction(env);
    auto const reader = db.reader(rtxn);
    auto const optData = reader.get(1);
    REQUIRE(optData.has_value());
    REQUIRE(utility::bytes::stringView(*optData) == "hello");
  }

  TEST_CASE("Database::Writer - create with id and size", "[lmdb][unit][database][writer]")
  {
    auto const temp = TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto db = openDatabase(wtxn, "test");
    auto writer = db.writer(wtxn);
    auto const result = writer.create(1, 10);
    REQUIRE(result);
    REQUIRE(!result->empty());
    REQUIRE(result->size() == 10);

    // Write data into the reserved space
    std::memset(result->data(), 'x', 10);

    REQUIRE(wtxn.commit());

    // Verify via reader
    auto const rtxn = beginReadTransaction(env);
    auto const reader = db.reader(rtxn);
    auto const optData = reader.get(1);
    REQUIRE(optData.has_value());
    REQUIRE(optData->size() == 10);
    REQUIRE(utility::bytes::stringView(*optData) == std::string(10, 'x'));
  }

  TEST_CASE("Database::Writer - append with data", "[lmdb][unit][database][writer]")
  {
    auto const temp = TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto db = openDatabase(wtxn, "test");
    auto writer = db.writer(wtxn);

    auto const id1 = writer.append(createStringData("first"));
    REQUIRE(id1);
    REQUIRE(*id1 == 1);

    auto const id2 = writer.append(createStringData("second"));
    REQUIRE(id2);
    REQUIRE(*id2 == 2);

    REQUIRE(wtxn.commit());

    // Verify via reader
    auto const rtxn = beginReadTransaction(env);
    auto const reader = db.reader(rtxn);
    auto const optData1 = reader.get(1);
    auto const optData2 = reader.get(2);
    REQUIRE(optData1.has_value());
    REQUIRE(optData2.has_value());
    REQUIRE(utility::bytes::stringView(*optData1) == "first");
    REQUIRE(utility::bytes::stringView(*optData2) == "second");
  }

  TEST_CASE("Database::Writer - append with size", "[lmdb][unit][database][writer]")
  {
    auto const temp = TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto db = openDatabase(wtxn, "test");
    auto writer = db.writer(wtxn);

    auto appendResult1 = writer.append(8);
    REQUIRE(appendResult1);
    auto const& [id1, result1] = *appendResult1;
    REQUIRE(id1 == 1);
    REQUIRE(!result1.empty());
    REQUIRE(result1.size() == 8);
    std::memset(result1.data(), 'a', 8);

    auto appendResult2 = writer.append(12);
    REQUIRE(appendResult2);
    auto const& [id2, result2] = *appendResult2;
    REQUIRE(id2 == 2);
    REQUIRE(!result2.empty());
    REQUIRE(result2.size() == 12);
    std::memset(result2.data(), 'b', 12);

    REQUIRE(wtxn.commit());

    // Verify via reader
    auto const rtxn = beginReadTransaction(env);
    auto const reader = db.reader(rtxn);
    auto const optData1 = reader.get(1);
    auto const optData2 = reader.get(2);
    REQUIRE(optData1.has_value());
    REQUIRE(optData2.has_value());
    REQUIRE(utility::bytes::stringView(*optData1) == std::string(8, 'a'));
    REQUIRE(utility::bytes::stringView(*optData2) == std::string(12, 'b'));
  }

  TEST_CASE("Database::Writer - update existing record", "[lmdb][unit][database][writer]")
  {
    auto const temp = TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto db = openDatabase(wtxn, "test");
    auto writer = db.writer(wtxn);

    // Create initial record
    REQUIRE(writer.create(1, createStringData("original")));
    REQUIRE(wtxn.commit());

    // Update the record
    auto wtxn2 = beginWriteTransaction(env);
    auto writer2 = db.writer(wtxn2);
    REQUIRE(writer2.update(1, createStringData("updated")));
    REQUIRE(wtxn2.commit());

    // Verify via reader
    auto const rtxn = beginReadTransaction(env);
    auto const reader = db.reader(rtxn);
    auto const optData = reader.get(1);
    REQUIRE(optData.has_value());
    REQUIRE(optData->size() == 7);
    REQUIRE(utility::bytes::stringView(*optData) == "updated");
  }

  TEST_CASE("Database::Writer - delete record", "[lmdb][unit][database][writer]")
  {
    auto const temp = TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto db = openDatabase(wtxn, "test");
    auto writer = db.writer(wtxn);
    REQUIRE(writer.create(1, createStringData("test")));
    REQUIRE(wtxn.commit());

    // Verify exists
    {
      auto const rtxn = beginReadTransaction(env);
      auto const reader = db.reader(rtxn);
      auto const optData1 = reader.get(1);
      REQUIRE(optData1.has_value());
    }

    // Delete
    {
      auto wtxn = beginWriteTransaction(env);
      auto writer = db.writer(wtxn);
      REQUIRE(writer.del(1));
      REQUIRE(wtxn.commit());
    }

    // Verify deleted
    {
      auto const rtxn = beginReadTransaction(env);
      auto const reader = db.reader(rtxn);
      auto const optData = reader.get(1);
      REQUIRE_FALSE(optData.has_value());
    }
  }

  TEST_CASE("Database::Writer - delete missing record returns false", "[lmdb][unit][database][writer]")
  {
    auto const temp = TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto db = openDatabase(wtxn, "test");
    auto writer = db.writer(wtxn);

    REQUIRE_FALSE(writer.del(123));
  }

  TEST_CASE("Database::Writer - get within write transaction", "[lmdb][unit][database][writer]")
  {
    auto const temp = TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto db = openDatabase(wtxn, "test");
    auto writer = db.writer(wtxn);

    REQUIRE(writer.create(42, createStringData("answer")));
    auto const optDataResult = writer.get(42);
    REQUIRE(optDataResult);
    REQUIRE(optDataResult->size() == 6);
    REQUIRE(utility::bytes::stringView(*optDataResult) == "answer");
  }

  TEST_CASE("Database::Writer - move constructor", "[lmdb][unit][database][writer]")
  {
    auto const temp = TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto db = openDatabase(wtxn, "test");

    auto writer1 = db.writer(wtxn);
    REQUIRE(writer1.create(1, createStringData("test")));

    auto writer2 = Database::Writer{std::move(writer1)};
    // writer1 is now in moved-from state
    // writer2 should still be usable

    REQUIRE(wtxn.commit());
  }

  TEST_CASE("Database::Writer - create returns Conflict on duplicate id with data", "[lmdb][unit][database][writer]")
  {
    auto const temp = TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto db = openDatabase(wtxn, "test");
    auto writer = db.writer(wtxn);

    REQUIRE(writer.create(1, createStringData("first")));

    auto const result = writer.create(1, createStringData("duplicate"));
    REQUIRE_FALSE(result);
    CHECK(result.error().code == Error::Code::Conflict);
    REQUIRE(wtxn.commit());
  }

  TEST_CASE("Database::Writer - create returns Conflict on duplicate id with size", "[lmdb][unit][database][writer]")
  {
    auto const temp = TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto db = openDatabase(wtxn, "test");
    auto writer = db.writer(wtxn);

    REQUIRE(writer.create(1, 10));

    auto const result = writer.create(1, 5);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == Error::Code::Conflict);
    REQUIRE(wtxn.commit());
  }

  TEST_CASE("Database::Reader - maxKey on empty database", "[lmdb][unit][database][reader]")
  {
    auto const temp = TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto db = openDatabase(wtxn, "test");
    REQUIRE(wtxn.commit());

    auto const rtxn = beginReadTransaction(env);
    auto const reader = db.reader(rtxn);
    REQUIRE(reader.maxKey() == 0);
  }

  TEST_CASE("Database::Reader - maxKey after append", "[lmdb][unit][database][reader]")
  {
    auto const temp = TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto db = openDatabase(wtxn, "test");
    auto writer = db.writer(wtxn);

    REQUIRE(writer.append(createStringData("first")));
    auto const id2 = writer.append(createStringData("second"));
    REQUIRE(id2);
    REQUIRE(wtxn.commit());

    auto const rtxn = beginReadTransaction(env);
    auto const reader = db.reader(rtxn);
    REQUIRE(reader.maxKey() == *id2);
  }

  TEST_CASE("Database::Reader - maxKey after create", "[lmdb][unit][database][reader]")
  {
    auto const temp = TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto db = openDatabase(wtxn, "test");
    auto writer = db.writer(wtxn);

    REQUIRE(writer.create(5, createStringData("five")));
    REQUIRE(writer.create(10, createStringData("ten")));
    REQUIRE(writer.create(3, createStringData("three")));
    REQUIRE(wtxn.commit());

    auto const rtxn = beginReadTransaction(env);
    auto const reader = db.reader(rtxn);
    REQUIRE(reader.maxKey() == 10);
  }

  TEST_CASE("Database::Reader - maxKey after delete", "[lmdb][unit][database][reader]")
  {
    auto const temp = TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto db = openDatabase(wtxn, "test");
    auto writer = db.writer(wtxn);

    REQUIRE(writer.create(1, createStringData("one")));
    REQUIRE(writer.create(2, createStringData("two")));
    REQUIRE(writer.create(3, createStringData("three")));
    REQUIRE(wtxn.commit());

    // Delete the max key
    {
      auto wtxn2 = beginWriteTransaction(env);
      auto writer2 = db.writer(wtxn2);
      REQUIRE(writer2.del(3));
      REQUIRE(wtxn2.commit());
    }

    auto const rtxn = beginReadTransaction(env);
    auto const reader = db.reader(rtxn);
    REQUIRE(reader.maxKey() == 2);
  }

  TEST_CASE("Database::Reader - maxKey after deleting middle element", "[lmdb][unit][database][reader]")
  {
    auto const temp = TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto db = openDatabase(wtxn, "test");
    auto writer = db.writer(wtxn);

    REQUIRE(writer.create(1, createStringData("one")));
    REQUIRE(writer.create(2, createStringData("two")));
    REQUIRE(writer.create(3, createStringData("three")));
    REQUIRE(wtxn.commit());

    // Delete the middle element
    {
      auto wtxn2 = beginWriteTransaction(env);
      auto writer2 = db.writer(wtxn2);
      REQUIRE(writer2.del(2));
      REQUIRE(wtxn2.commit());
    }

    auto const rtxn = beginReadTransaction(env);
    auto const reader = db.reader(rtxn);
    // maxKey should still be 3 (the max wasn't deleted)
    REQUIRE(reader.maxKey() == 3);
  }

  TEST_CASE("Database - Blob keys", "[lmdb][unit][database][blob]")
  {
    auto const temp = TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto db = openDatabase(wtxn, "blobdb", Database::KeyKind::Blob);
    auto writer = db.writer(wtxn);

    auto const key1 = createStringData("key1");
    auto const key2 = createStringData("another_key");
    auto const val1 = createStringData("value1");
    auto const val2 = createStringData("value2");

    REQUIRE(writer.create(key1, val1));
    REQUIRE(writer.create(key2, val2));
    REQUIRE(wtxn.commit());

    auto const rtxn = beginReadTransaction(env);
    auto const reader = db.reader(rtxn);

    SECTION("get works with blob keys")
    {
      auto const optRes1 = reader.get(key1);
      REQUIRE(optRes1);
      REQUIRE(utility::bytes::stringView(*optRes1) == "value1");

      auto const optRes2 = reader.get(key2);
      REQUIRE(optRes2);
      REQUIRE(utility::bytes::stringView(*optRes2) == "value2");
    }

    SECTION("Iteration works with blob keys")
    {
      auto it = reader.begin();
      REQUIRE(it != reader.end());
      // LMDB sorts lexicographically
      REQUIRE(utility::bytes::stringView(it->first) == "another_key");
      REQUIRE(utility::bytes::stringView(it->second) == "value2");

      ++it;
      REQUIRE(it != reader.end());
      REQUIRE(utility::bytes::stringView(it->first) == "key1");
      REQUIRE(utility::bytes::stringView(it->second) == "value1");

      ++it;
      REQUIRE(it == reader.end());
    }

    SECTION("Writer::get works with blob keys")
    {
      auto wtxn2 = beginWriteTransaction(env);
      auto writer2 = db.writer(wtxn2);
      auto const optRes = writer2.get(key1);
      REQUIRE(optRes);
      REQUIRE(utility::bytes::stringView(*optRes) == "value1");
    }

    SECTION("Writer::del works with blob keys")
    {
      auto wtxn2 = beginWriteTransaction(env);
      auto writer2 = db.writer(wtxn2);
      REQUIRE(writer2.del(key1));
      REQUIRE_FALSE(writer2.get(key1).has_value());
      REQUIRE(wtxn2.commit());
    }
  }

  TEST_CASE("Database::Writer - throws when used after commit", "[lmdb][unit][database][writer]")
  {
    auto const temp = TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto db = openDatabase(wtxn, "test");
    auto writer = db.writer(wtxn);
    REQUIRE(writer.create(1, createStringData("before")));
    REQUIRE(wtxn.commit());

    // After commit LMDB has closed the writer's cursor; every operation must
    // throw instead of dereferencing the dangling handle.
    REQUIRE_THROWS_AS(writer.create(2, createStringData("after")), Exception);
    REQUIRE_THROWS_AS(writer.update(1, createStringData("after")), Exception);
    REQUIRE_THROWS_AS(writer.del(1), Exception);
    REQUIRE_THROWS_AS(writer.get(1), Exception);
    REQUIRE_THROWS_AS(writer.clear(), Exception);
  }

  TEST_CASE("Database::Reader::KeyView - coercion throws on non-uint32 key", "[lmdb][unit][database][reader]")
  {
    auto const temp = TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto db = openDatabase(wtxn, "blobdb", Database::KeyKind::Blob);
    auto writer = db.writer(wtxn);
    REQUIRE(writer.create(createStringData("xy"), createStringData("value"))); // 2-byte key
    REQUIRE(wtxn.commit());

    auto const rtxn = beginReadTransaction(env);
    auto const reader = db.reader(rtxn);
    auto const it = reader.begin();
    REQUIRE(it != reader.end());

    // A 2-byte key cannot be coerced to uint32; it must throw rather than yield 0.
    REQUIRE_THROWS_AS(static_cast<std::uint32_t>(it->first), Exception);
  }
} // namespace ao::lmdb::test
