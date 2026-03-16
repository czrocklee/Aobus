// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <catch2/catch.hpp>

#include <rs/core/Dictionary.h>
#include <rs/lmdb/Database.h>
#include <rs/lmdb/Environment.h>
#include <rs/lmdb/Transaction.h>
#include <test/lmdb/LmdbTestUtils.h>

#include <string_view>

using rs::core::Dictionary;
using rs::core::DictionaryId;
using rs::lmdb::Database;
using rs::lmdb::Environment;
using rs::lmdb::ReadTransaction;
using rs::lmdb::test::TempDir;
using rs::lmdb::WriteTransaction;

TEST_CASE("Dictionary - store and get", "[core][dictionary]")
{
  TempDir temp;
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  WriteTransaction wtxn(env);
  Dictionary dict{wtxn, "dict"};
  wtxn.commit();

  // Store a value
  WriteTransaction wtxn2(env);
  auto id = dict.put(wtxn2, "test value");
  // If put() failed, it would throw
  wtxn2.commit();

  // Get by ID (using in-memory index)
  auto result = dict.get(id);
  REQUIRE(result == "test value");
}

TEST_CASE("Dictionary - getId", "[core][dictionary]")
{
  TempDir temp;
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  WriteTransaction wtxn(env);
  Dictionary dict{wtxn, "dict"};
  dict.put(wtxn, "artist1");
  wtxn.commit();

  // Get ID by string
  [[maybe_unused]] auto id = dict.getId("artist1");
  // If getId() failed, it would throw
}

TEST_CASE("Dictionary - contains by string", "[core][dictionary]")
{
  TempDir temp;
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  WriteTransaction wtxn(env);
  Dictionary dict{wtxn, "dict"};
  dict.put(wtxn, "exists");
  wtxn.commit();

  REQUIRE(dict.contains("exists"));
  REQUIRE(!dict.contains("not exists"));
}

TEST_CASE("Dictionary - put duplicate string returns existing ID", "[core][dictionary]")
{
  TempDir temp;
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  WriteTransaction wtxn(env);
  Dictionary dict{wtxn, "dict"};
  wtxn.commit();

  // Store a value
  WriteTransaction wtxn2(env);
  auto id1 = dict.put(wtxn2, "first");
  // If put() failed, it would throw
  wtxn2.commit();

  // Try to store same string again - returns existing ID
  WriteTransaction wtxn3(env);
  auto id2 = dict.put(wtxn3, "first");
  REQUIRE(id2 == id1);

  // Original value should still exist (using in-memory index)
  auto result = dict.get(id1);
  REQUIRE(result == "first");
}

TEST_CASE("Dictionary - get throws on invalid ID", "[core][dictionary]")
{
  TempDir temp;
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  WriteTransaction wtxn(env);
  Dictionary dict{wtxn, "dict"};
  dict.put(wtxn, "first");
  wtxn.commit();

  // ID 999 doesn't exist - should throw
  CHECK_THROWS(dict.get(DictionaryId{999}));
}

TEST_CASE("Dictionary - getId throws on non-existent string", "[core][dictionary]")
{
  TempDir temp;
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  WriteTransaction wtxn(env);
  Dictionary dict{wtxn, "dict"};
  dict.put(wtxn, "exists");
  wtxn.commit();

  // Non-existent string should throw
  CHECK_THROWS(dict.getId("not exists"));
}

TEST_CASE("Dictionary - get with first valid ID (0)", "[core][dictionary]")
{
  TempDir temp;
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  WriteTransaction wtxn(env);
  Dictionary dict{wtxn, "dict"};
  auto id = dict.put(wtxn, "first");
  wtxn.commit();

  REQUIRE(id.value() == 0);
  auto result = dict.get(id);
  REQUIRE(result == "first");
}

TEST_CASE("Dictionary - get throws on out-of-bounds ID", "[core][dictionary]")
{
  TempDir temp;
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  WriteTransaction wtxn(env);
  Dictionary dict{wtxn, "dict"};
  dict.put(wtxn, "only one");
  wtxn.commit();

  // ID 1 is out of bounds (only 0 is valid)
  CHECK_THROWS(dict.get(DictionaryId{1}));
}
