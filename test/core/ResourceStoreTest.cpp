// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <catch2/catch.hpp>

#include <rs/core/ResourceStore.h>
#include <rs/lmdb/Database.h>
#include <rs/lmdb/Environment.h>
#include <rs/lmdb/Transaction.h>
#include <test/lmdb/LmdbTestUtils.h>

using rs::core::ResourceStore;
using rs::lmdb::Database;
using rs::lmdb::Environment;
using rs::lmdb::ReadTransaction;
using rs::lmdb::WriteTransaction;

TEST_CASE("ResourceStore - create and read", "[core][resource]")
{
  TempDir temp;
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  WriteTransaction wtxn(env);
  ResourceStore store{wtxn, "resources"};
  wtxn.commit();

  // Create a resource
  auto data = createStringData("hello");
  auto buffer = makeBuffer(data);

  WriteTransaction wtxn2(env);
  auto id = store.writer(wtxn2).create(buffer);
  REQUIRE(id > 0);
  wtxn2.commit();

  // Read the resource
  ReadTransaction rtxn(env);
  auto reader = store.reader(rtxn);
  auto it = reader.begin();
  REQUIRE(it != reader.end());
  REQUIRE(it->first == id);
}

TEST_CASE("ResourceStore - delete", "[core][resource]")
{
  TempDir temp;
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  WriteTransaction wtxn(env);
  ResourceStore store{wtxn, "resources"};
  wtxn.commit();

  // Create a resource
  auto data = createStringData("test");
  auto buffer = makeBuffer(data);

  WriteTransaction wtxn2(env);
  auto id = store.writer(wtxn2).create(buffer);
  wtxn2.commit();

  // Delete it
  WriteTransaction wtxn3(env);
  auto deleted = store.writer(wtxn3).del(id);
  REQUIRE(deleted);
  wtxn3.commit();

  // Verify it's gone
  ReadTransaction rtxn(env);
  auto reader = store.reader(rtxn);
  auto it = reader.begin();
  REQUIRE(it == reader.end());
}

TEST_CASE("ResourceStore - deduplication", "[core][resource]")
{
  TempDir temp;
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  WriteTransaction wtxn(env);
  ResourceStore store{wtxn, "resources"};
  wtxn.commit();

  // Create first resource
  auto data = ::createStringData("samedata");
  auto buffer = makeBuffer(data);

  WriteTransaction wtxn2(env);
  auto id1 = store.writer(wtxn2).create(buffer);
  wtxn2.commit();

  // Create same content again - should return same ID (deduplication)
  WriteTransaction wtxn3(env);
  auto id2 = store.writer(wtxn3).create(buffer);
  REQUIRE(id2 == id1);
  wtxn3.commit();

  // Verify only one resource exists
  ReadTransaction rtxn(env);
  auto reader = store.reader(rtxn);
  int count = 0;
  for (auto it = reader.begin(); it != reader.end(); ++it)
  {
    ++count;
  }
  REQUIRE(count == 1);
}
