// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <catch2/catch.hpp>

#include <rs/core/ListLayout.h>
#include <rs/core/ListStore.h>
#include <rs/lmdb/Database.h>
#include <rs/lmdb/Environment.h>
#include <rs/lmdb/Transaction.h>
#include <test/lmdb/TestUtils.h>

#include <cstring>
#include <vector>

using rs::core::ListHeader;
using rs::core::ListStore;
using rs::core::ListView;
using rs::lmdb::Database;
using rs::lmdb::Environment;
using rs::lmdb::ReadTransaction;
using rs::lmdb::WriteTransaction;

TEST_CASE("ListStore - create and read", "[core][list]")
{
  auto temp = TempDir{};
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  auto wtxn = WriteTransaction{env};
  auto store = ListStore{wtxn, "lists"};
  wtxn.commit();

  // Create a list
  ListHeader header{};
  header.trackIdsCount = 5;

  std::vector<std::byte> data(sizeof(ListHeader));
  std::memcpy(data.data(), &header, sizeof(ListHeader));

  auto wtxn2 = WriteTransaction{env};
  auto [id, view] = store.writer(wtxn2).create(data);
  // If create() failed, it would throw
  wtxn2.commit();

  // Read the list
  auto rtxn = ReadTransaction{env};
  auto reader = store.reader(rtxn);
  auto it = reader.begin();
  REQUIRE(it != reader.end());
  REQUIRE((*it).first == id);
}

TEST_CASE("ListStore - read by id", "[core][list]")
{
  auto temp = TempDir{};
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  auto wtxn = WriteTransaction{env};
  auto store = ListStore{wtxn, "lists"};
  wtxn.commit();

  // Create a list
  ListHeader header{};
  header.trackIdsCount = 10;

  std::vector<std::byte> data(sizeof(ListHeader));
  std::memcpy(data.data(), &header, sizeof(ListHeader));

  auto wtxn2 = WriteTransaction{env};
  auto [id, view] = store.writer(wtxn2).create(data);
  wtxn2.commit();

  // Read by ID
  auto rtxn = ReadTransaction{env};
  auto optFound = store.reader(rtxn).get(id);
  REQUIRE(optFound.has_value());
  auto& found = *optFound;
  REQUIRE(found.header()->trackIdsCount == 10);
}

TEST_CASE("ListStore - delete", "[core][list]")
{
  auto temp = TempDir{};
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  auto wtxn = WriteTransaction{env};
  auto store = ListStore{wtxn, "lists"};
  wtxn.commit();

  // Create a list
  ListHeader header{};

  std::vector<std::byte> data(sizeof(ListHeader));
  std::memcpy(data.data(), &header, sizeof(ListHeader));

  auto wtxn2 = WriteTransaction{env};
  auto [id, view] = store.writer(wtxn2).create(data);
  wtxn2.commit();

  // Delete it
  auto wtxn3 = WriteTransaction{env};
  auto deleted = store.writer(wtxn3).del(id);
  REQUIRE(deleted);
  wtxn3.commit();

  // Verify it's gone
  auto rtxn = ReadTransaction{env};
  auto reader = store.reader(rtxn);
  auto it = reader.begin();
  REQUIRE(it == reader.end());
}
