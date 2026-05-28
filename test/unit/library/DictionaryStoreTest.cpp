// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "test/unit/lmdb/TestUtils.h"
#include <ao/Type.h>
#include <ao/library/DictionaryStore.h>
#include <ao/lmdb/Database.h>
#include <ao/lmdb/Environment.h>
#include <ao/lmdb/Transaction.h>
#include <ao/utility/ByteView.h>

#include <catch2/catch_test_macros.hpp>
#include <lmdb.h>

#include <cstdint>
#include <string>
#include <string_view>

using namespace std::string_view_literals;

namespace ao::library::test
{
  using namespace ao::lmdb;
  using namespace ao::lmdb::test;

  TEST_CASE("Dictionary - store and get", "[library][unit][dictionary]")
  {
    auto const temp = TempDir{};
    auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

    auto wtxn = WriteTransaction{env};
    auto dict = DictionaryStore{Database{wtxn, "dict"}, wtxn};
    wtxn.commit();

    // Store a value
    auto wtxn2 = WriteTransaction{env};
    auto const id = dict.put(wtxn2, "test value");
    // If put() failed, it would throw
    wtxn2.commit();

    // Get by ID (using in-memory index)
    auto const result = dict.get(id);
    REQUIRE(result == "test value");
  }

  TEST_CASE("Dictionary - getId", "[library][unit][dictionary]")
  {
    auto const temp = TempDir{};
    auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

    auto wtxn = WriteTransaction{env};
    auto dict = DictionaryStore{Database{wtxn, "dict"}, wtxn};
    dict.put(wtxn, "artist1");
    wtxn.commit();

    // Get ID by string
    [[maybe_unused]] auto const id = dict.getId("artist1");
    // If getId() failed, it would throw
  }

  TEST_CASE("Dictionary - contains by string", "[library][unit][dictionary]")
  {
    auto const temp = TempDir{};
    auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

    auto wtxn = WriteTransaction{env};
    auto dict = DictionaryStore{Database{wtxn, "dict"}, wtxn};
    dict.put(wtxn, "exists");
    wtxn.commit();

    REQUIRE(dict.contains("exists"));
    REQUIRE(!dict.contains("not exists"));
  }

  TEST_CASE("Dictionary - put duplicate string returns existing ID", "[library][unit][dictionary]")
  {
    auto const temp = TempDir{};
    auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

    auto wtxn = WriteTransaction{env};
    auto dict = DictionaryStore{Database{wtxn, "dict"}, wtxn};
    wtxn.commit();

    // Store a value
    auto wtxn2 = WriteTransaction{env};
    auto const id1 = dict.put(wtxn2, "first");
    // If put() failed, it would throw
    wtxn2.commit();

    // Try to store same string again - returns existing ID
    auto wtxn3 = WriteTransaction{env};
    auto const id2 = dict.put(wtxn3, "first");
    REQUIRE(id2 == id1);

    // Original value should still exist (using in-memory index)
    auto const result = dict.get(id1);
    REQUIRE(result == "first");
  }

  TEST_CASE("Dictionary - get throws on invalid ID", "[library][unit][dictionary]")
  {
    auto const temp = TempDir{};
    auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

    auto wtxn = WriteTransaction{env};
    auto dict = DictionaryStore{Database{wtxn, "dict"}, wtxn};
    dict.put(wtxn, "first");
    wtxn.commit();

    // ID 999 doesn't exist - should throw
    CHECK_THROWS(dict.get(DictionaryId{999}));
  }

  TEST_CASE("Dictionary - getId throws on non-existent string", "[library][unit][dictionary]")
  {
    auto const temp = TempDir{};
    auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

    auto wtxn = WriteTransaction{env};
    auto dict = DictionaryStore{Database{wtxn, "dict"}, wtxn};
    dict.put(wtxn, "exists");
    wtxn.commit();

    // Non-existent string should throw
    CHECK_THROWS(dict.getId("not exists"));
  }

  TEST_CASE("Dictionary - get with first valid ID (0)", "[library][unit][dictionary]")
  {
    auto const temp = TempDir{};
    auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

    auto wtxn = WriteTransaction{env};
    auto dict = DictionaryStore{Database{wtxn, "dict"}, wtxn};
    auto const id = dict.put(wtxn, "first");
    wtxn.commit();

    REQUIRE(id.raw() == 1);
    auto const result = dict.get(id);
    REQUIRE(result == "first");
  }

  TEST_CASE("Dictionary - get throws on out-of-bounds ID", "[library][unit][dictionary]")
  {
    auto const temp = TempDir{};
    auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

    auto wtxn = WriteTransaction{env};
    auto dict = DictionaryStore{Database{wtxn, "dict"}, wtxn};
    dict.put(wtxn, "only one");
    wtxn.commit();

    // ID 2 is out of bounds (only 1 is valid)
    CHECK_THROWS(dict.get(DictionaryId{2}));
  }

  TEST_CASE("Dictionary - getOrIntern returns new ID for non-existent string", "[library][unit][dictionary]")
  {
    auto const temp = TempDir{};
    auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

    auto wtxn = WriteTransaction{env};
    auto dict = DictionaryStore{Database{wtxn, "dict"}, wtxn};
    wtxn.commit();

    // Reserve a non-existent string
    auto const id = dict.getOrIntern("new artist");
    REQUIRE(id.raw() == 1); // First getOrInternd ID is 0 (same as put)

    // contains should now return true (in-memory)
    REQUIRE(dict.contains("new artist"));
  }

  TEST_CASE("Dictionary - getOrIntern returns existing ID for existent string", "[library][unit][dictionary]")
  {
    auto const temp = TempDir{};
    auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

    auto wtxn = WriteTransaction{env};
    auto dict = DictionaryStore{Database{wtxn, "dict"}, wtxn};
    dict.put(wtxn, "existing");
    wtxn.commit();

    // Reserve an existing string - should return the existing ID
    auto const id = dict.getOrIntern("existing");
    REQUIRE(id.raw() == 1); // First put uses ID 0

    REQUIRE(dict.contains("existing"));
  }

  TEST_CASE("Dictionary - getOrIntern then put returns same ID", "[library][unit][dictionary]")
  {
    auto const temp = TempDir{};
    auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

    auto wtxn = WriteTransaction{env};
    auto dict = DictionaryStore{Database{wtxn, "dict"}, wtxn};
    wtxn.commit();

    // Reserve a string (not persisted)
    auto const getOrInterndId = dict.getOrIntern("Bach");

    // put() should return the same ID
    auto wtxn2 = WriteTransaction{env};
    auto const putId = dict.put(wtxn2, "Bach");
    REQUIRE(putId == getOrInterndId);
  }

  TEST_CASE("Dictionary - getOrIntern multiple strings", "[library][unit][dictionary]")
  {
    auto const temp = TempDir{};
    auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

    auto wtxn = WriteTransaction{env};
    auto dict = DictionaryStore{Database{wtxn, "dict"}, wtxn};
    wtxn.commit();

    auto const id1 = dict.getOrIntern("artist1");
    auto const id2 = dict.getOrIntern("artist2");
    auto const id3 = dict.getOrIntern("artist3");

    REQUIRE(id1 != id2);
    REQUIRE(id2 != id3);
    REQUIRE(id3 != id1);

    REQUIRE(dict.contains("artist1"));
    REQUIRE(dict.contains("artist2"));
    REQUIRE(dict.contains("artist3"));
  }

  TEST_CASE("Dictionary - put different string after getOrIntern does not collide", "[library][unit][dictionary]")
  {
    auto const temp = TempDir{};
    auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

    auto wtxn = WriteTransaction{env};
    auto dict = DictionaryStore{Database{wtxn, "dict"}, wtxn};
    wtxn.commit();

    // Simulate scan populating some entries first
    auto wtxn2 = WriteTransaction{env};
    dict.put(wtxn2, "artist_one");
    dict.put(wtxn2, "album_one");
    wtxn2.commit();

    // getOrIntern reserves "fav" (like query compiler does for "#fav" smart list)
    auto const reservedId = dict.getOrIntern("fav");
    REQUIRE(reservedId.raw() == 3); // after 2 puts, next is 3

    // put a DIFFERENT string "#fav" — must NOT collide with reserved "fav"
    auto wtxn3 = WriteTransaction{env};
    auto const putId = dict.put(wtxn3, "#fav");
    wtxn3.commit();

    REQUIRE(putId.raw() != reservedId.raw());
    REQUIRE(putId.raw() == 4); // must skip over reserved ID 3

    // Both strings should be independently resolvable
    REQUIRE(dict.get(reservedId) == "fav");
    REQUIRE(dict.get(putId) == "#fav");
    REQUIRE(dict.getId("fav").raw() == 3);
    REQUIRE(dict.getId("#fav").raw() == 4);
  }

  TEST_CASE("Dictionary - put same string as getOrIntern returns reserved ID and persists",
            "[library][unit][dictionary]")
  {
    auto const temp = TempDir{};
    auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

    auto wtxn = WriteTransaction{env};
    auto dict = DictionaryStore{Database{wtxn, "dict"}, wtxn};
    wtxn.commit();

    // Reserve a string
    auto const reservedId = dict.getOrIntern("fav");
    REQUIRE(reservedId.raw() == 1);

    // Put the SAME string — should reuse the reserved ID and persist it
    auto wtxn2 = WriteTransaction{env};
    auto const putId = dict.put(wtxn2, "fav");
    wtxn2.commit();

    REQUIRE(putId.raw() == reservedId.raw());
    REQUIRE(dict.get(putId) == "fav");

    // After restart simulation: reload dictionary from DB
    auto rtxn = ReadTransaction{env};
    auto dict2 = DictionaryStore{Database{rtxn, "dict"}, rtxn};
    REQUIRE(dict2.get(putId) == "fav");
  }

  TEST_CASE("Dictionary - getOrDefault returns value for valid ID", "[library][unit][dictionary]")
  {
    auto const temp = TempDir{};
    auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

    auto wtxn = WriteTransaction{env};
    auto dict = DictionaryStore{Database{wtxn, "dict"}, wtxn};
    auto const id = dict.put(wtxn, "hello");
    wtxn.commit();

    CHECK(dict.getOrDefault(id) == "hello");
    CHECK(dict.getOrDefault(id, "fallback") == "hello");
  }

  TEST_CASE("Dictionary - getOrDefault returns default for invalid ID", "[library][unit][dictionary]")
  {
    auto const temp = TempDir{};
    auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

    auto wtxn = WriteTransaction{env};
    auto dict = DictionaryStore{Database{wtxn, "dict"}, wtxn};
    wtxn.commit();

    CHECK(dict.getOrDefault(kInvalidDictionaryId).empty());
    CHECK(dict.getOrDefault(kInvalidDictionaryId, "fallback") == "fallback");
    CHECK(dict.getOrDefault(DictionaryId{999}).empty());
  }

  TEST_CASE("Dictionary - handles gaps in database IDs correctly", "[library][unit][dictionary]")
  {
    auto const temp = TempDir{};
    auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

    // Manually create a gap in the database
    {
      auto wtxn = WriteTransaction{env};
      auto db = Database{wtxn, "dict"};
      auto writer = db.writer(wtxn);

      writer.create(1, utility::bytes::view("first"sv));
      // SKIP ID 2
      writer.create(3, utility::bytes::view("third"sv));
      // SKIP ID 4
      writer.create(5, utility::bytes::view("fifth"sv));

      wtxn.commit();
    }

    // Load DictionaryStore which should correctly handle the gaps via resize()
    auto rtxn = ReadTransaction{env};
    auto dict = DictionaryStore{Database{rtxn, "dict"}, rtxn};

    REQUIRE(dict.size() == 3);

    // Valid entries
    CHECK(dict.get(DictionaryId{1}) == "first");
    CHECK(dict.get(DictionaryId{3}) == "third");
    CHECK(dict.get(DictionaryId{5}) == "fifth");

    // Gapped entries should return empty strings but NOT throw out-of-bounds
    CHECK(dict.get(DictionaryId{2}).empty());
    CHECK(dict.get(DictionaryId{4}).empty());

    // Transparent index should also resolve correctly
    CHECK(dict.getId("first").raw() == 1);
    CHECK(dict.getId("fifth").raw() == 5);
  }

  TEST_CASE("Dictionary - recycles gap IDs across restarts", "[library][unit][dictionary]")
  {
    auto const temp = TempDir{};
    auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

    // Manually create a gap in the database
    {
      auto wtxn = WriteTransaction{env};
      auto db = Database{wtxn, "dict"};
      auto writer = db.writer(wtxn);

      writer.create(1, utility::bytes::view("first"sv));
      // SKIP ID 2
      writer.create(3, utility::bytes::view("third"sv));

      wtxn.commit();
    }

    // Load DictionaryStore which should discover ID 2 as a gap
    auto wtxn2 = WriteTransaction{env};
    auto dict = DictionaryStore{Database{wtxn2, "dict"}, wtxn2};

    // Inserting a new string should RECYCLE ID 2 instead of appending to 4
    auto const newId1 = dict.put(wtxn2, "recycled_id_2");
    REQUIRE(newId1.raw() == 2);

    // Inserting another new string should go to 4 since gaps are exhausted
    auto const newId2 = dict.getOrIntern("appended_id_4");
    REQUIRE(newId2.raw() == 4);

    // Inserting another should go to 5
    auto const newId3 = dict.put(wtxn2, "appended_id_5");
    REQUIRE(newId3.raw() == 5);

    wtxn2.commit();
  }

  TEST_CASE("Dictionary - prevents dangling pointers upon heavy reallocation", "[library][unit][dictionary]")
  {
    auto const temp = TempDir{};
    auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

    auto wtxn = WriteTransaction{env};
    auto dict = DictionaryStore{Database{wtxn, "dict"}, wtxn};
    wtxn.commit();

    // Store the first string, capturing its ID
    auto wtxn2 = WriteTransaction{env};
    auto const firstId = dict.put(wtxn2, "very_first_string");
    wtxn2.commit();

    // Insert 10,000 unique short strings to force vector reallocation multiple times
    auto wtxn3 = WriteTransaction{env};

    for (std::int32_t i = 0; i < 10000; ++i)
    {
      dict.put(wtxn3, "string_" + std::to_string(i));
    }

    wtxn3.commit();

    // After massive reallocation, the old string_view in the transparent index should NOT dangle,
    // because we use DictionaryId in the hash set and vector element direct lookup.
    // So looking up "very_first_string" should succeed and return the correct ID.
    REQUIRE_NOTHROW(dict.getId("very_first_string"));
    REQUIRE(dict.getId("very_first_string") == firstId);

    // And get() should resolve correctly
    REQUIRE(dict.get(firstId) == "very_first_string");
  }
} // namespace ao::library::test
