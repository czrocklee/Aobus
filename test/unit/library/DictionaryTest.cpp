// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators_all.hpp>
#include <catch2/matchers/catch_matchers_all.hpp>

#include <ao/library/DictionaryStore.h>
#include <ao/lmdb/Database.h>
#include <ao/lmdb/Environment.h>
#include <ao/lmdb/Transaction.h>
#include <test/unit/lmdb/TestUtils.h>

#include <string_view>

using ao::DictionaryId;
using ao::library::DictionaryStore;
using ao::lmdb::Database;
using ao::lmdb::Environment;
using ao::lmdb::ReadTransaction;
using ao::lmdb::WriteTransaction;

TEST_CASE("Dictionary - store and get", "[core][dictionary]")
{
  auto temp = TempDir{};
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  auto wtxn = WriteTransaction{env};
  auto dict = DictionaryStore{Database{wtxn, "dict"}, wtxn};
  wtxn.commit();

  // Store a value
  auto wtxn2 = WriteTransaction{env};
  auto id = dict.put(wtxn2, "test value");
  // If put() failed, it would throw
  wtxn2.commit();

  // Get by ID (using in-memory index)
  auto result = dict.get(id);
  REQUIRE(result == "test value");
}

TEST_CASE("Dictionary - getId", "[core][dictionary]")
{
  auto temp = TempDir{};
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  auto wtxn = WriteTransaction{env};
  auto dict = DictionaryStore{Database{wtxn, "dict"}, wtxn};
  dict.put(wtxn, "artist1");
  wtxn.commit();

  // Get ID by string
  [[maybe_unused]] auto id = dict.getId("artist1");
  // If getId() failed, it would throw
}

TEST_CASE("Dictionary - contains by string", "[core][dictionary]")
{
  auto temp = TempDir{};
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  auto wtxn = WriteTransaction{env};
  auto dict = DictionaryStore{Database{wtxn, "dict"}, wtxn};
  dict.put(wtxn, "exists");
  wtxn.commit();

  REQUIRE(dict.contains("exists"));
  REQUIRE(!dict.contains("not exists"));
}

TEST_CASE("Dictionary - put duplicate string returns existing ID", "[core][dictionary]")
{
  auto temp = TempDir{};
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  auto wtxn = WriteTransaction{env};
  auto dict = DictionaryStore{Database{wtxn, "dict"}, wtxn};
  wtxn.commit();

  // Store a value
  auto wtxn2 = WriteTransaction{env};
  auto id1 = dict.put(wtxn2, "first");
  // If put() failed, it would throw
  wtxn2.commit();

  // Try to store same string again - returns existing ID
  auto wtxn3 = WriteTransaction{env};
  auto id2 = dict.put(wtxn3, "first");
  REQUIRE(id2 == id1);

  // Original value should still exist (using in-memory index)
  auto result = dict.get(id1);
  REQUIRE(result == "first");
}

TEST_CASE("Dictionary - get throws on invalid ID", "[core][dictionary]")
{
  auto temp = TempDir{};
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  auto wtxn = WriteTransaction{env};
  auto dict = DictionaryStore{Database{wtxn, "dict"}, wtxn};
  dict.put(wtxn, "first");
  wtxn.commit();

  // ID 999 doesn't exist - should throw
  CHECK_THROWS(dict.get(DictionaryId{999}));
}

TEST_CASE("Dictionary - getId throws on non-existent string", "[core][dictionary]")
{
  auto temp = TempDir{};
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  auto wtxn = WriteTransaction{env};
  auto dict = DictionaryStore{Database{wtxn, "dict"}, wtxn};
  dict.put(wtxn, "exists");
  wtxn.commit();

  // Non-existent string should throw
  CHECK_THROWS(dict.getId("not exists"));
}

TEST_CASE("Dictionary - get with first valid ID (0)", "[core][dictionary]")
{
  auto temp = TempDir{};
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  auto wtxn = WriteTransaction{env};
  auto dict = DictionaryStore{Database{wtxn, "dict"}, wtxn};
  auto id = dict.put(wtxn, "first");
  wtxn.commit();

  REQUIRE(id.value() == 1);
  auto result = dict.get(id);
  REQUIRE(result == "first");
}

TEST_CASE("Dictionary - get throws on out-of-bounds ID", "[core][dictionary]")
{
  auto temp = TempDir{};
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  auto wtxn = WriteTransaction{env};
  auto dict = DictionaryStore{Database{wtxn, "dict"}, wtxn};
  dict.put(wtxn, "only one");
  wtxn.commit();

  // ID 2 is out of bounds (only 1 is valid)
  CHECK_THROWS(dict.get(DictionaryId{2}));
}

TEST_CASE("Dictionary - getOrIntern returns new ID for non-existent string", "[core][dictionary]")
{
  auto temp = TempDir{};
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  auto wtxn = WriteTransaction{env};
  auto dict = DictionaryStore{Database{wtxn, "dict"}, wtxn};
  wtxn.commit();

  // Reserve a non-existent string
  auto id = dict.getOrIntern("new artist");
  REQUIRE(id.value() == 1); // First getOrInternd ID is 0 (same as put)

  // contains should now return true (in-memory)
  REQUIRE(dict.contains("new artist"));
}

TEST_CASE("Dictionary - getOrIntern returns existing ID for existent string", "[core][dictionary]")
{
  auto temp = TempDir{};
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  auto wtxn = WriteTransaction{env};
  auto dict = DictionaryStore{Database{wtxn, "dict"}, wtxn};
  dict.put(wtxn, "existing");
  wtxn.commit();

  // Reserve an existing string - should return the existing ID
  auto id = dict.getOrIntern("existing");
  REQUIRE(id.value() == 1); // First put uses ID 0

  REQUIRE(dict.contains("existing"));
}

TEST_CASE("Dictionary - getOrIntern then put returns same ID", "[core][dictionary]")
{
  auto temp = TempDir{};
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  auto wtxn = WriteTransaction{env};
  auto dict = DictionaryStore{Database{wtxn, "dict"}, wtxn};
  wtxn.commit();

  // Reserve a string (not persisted)
  auto getOrInterndId = dict.getOrIntern("Bach");

  // put() should return the same ID
  auto wtxn2 = WriteTransaction{env};
  auto putId = dict.put(wtxn2, "Bach");
  REQUIRE(putId == getOrInterndId);
}

TEST_CASE("Dictionary - getOrIntern multiple strings", "[core][dictionary]")
{
  auto temp = TempDir{};
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  auto wtxn = WriteTransaction{env};
  auto dict = DictionaryStore{Database{wtxn, "dict"}, wtxn};
  wtxn.commit();

  auto id1 = dict.getOrIntern("artist1");
  auto id2 = dict.getOrIntern("artist2");
  auto id3 = dict.getOrIntern("artist3");

  REQUIRE(id1 != id2);
  REQUIRE(id2 != id3);
  REQUIRE(id3 != id1);

  REQUIRE(dict.contains("artist1"));
  REQUIRE(dict.contains("artist2"));
  REQUIRE(dict.contains("artist3"));
}
