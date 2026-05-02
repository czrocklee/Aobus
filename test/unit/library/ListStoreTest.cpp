// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators_all.hpp>
#include <catch2/matchers/catch_matchers_all.hpp>

#include <ao/library/ListLayout.h>
#include <ao/library/ListStore.h>
#include <ao/lmdb/Database.h>
#include <ao/lmdb/Environment.h>
#include <ao/lmdb/Transaction.h>
#include <test/unit/lmdb/TestUtils.h>

#include <cstring>
#include <vector>

using ao::library::ListHeader;
using ao::library::ListStore;
using ao::library::ListView;
using ao::lmdb::Database;
using ao::lmdb::Environment;
using ao::lmdb::ReadTransaction;
using ao::lmdb::WriteTransaction;

TEST_CASE("ListStore - create and read", "[core][list]")
{
  auto temp = TempDir{};
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  auto wtxn = WriteTransaction{env};
  auto store = ListStore{Database{wtxn, "lists"}};
  wtxn.commit();

  // Create a list
  auto header = ListHeader{};
  header.trackIdsCount = 5;

  auto data = std::vector<std::byte>(sizeof(ListHeader));
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
  auto store = ListStore{Database{wtxn, "lists"}};
  wtxn.commit();

  // Create a list
  auto header = ListHeader{};
  header.trackIdsCount = 10;

  auto const trackIdsSize = static_cast<std::size_t>(header.trackIdsCount) * sizeof(ao::TrackId);
  auto data = std::vector<std::byte>(sizeof(ListHeader) + trackIdsSize);
  std::memcpy(data.data(), &header, sizeof(ListHeader));

  auto wtxn2 = WriteTransaction{env};
  auto [id, view] = store.writer(wtxn2).create(data);
  wtxn2.commit();

  // Read by ID
  auto rtxn = ReadTransaction{env};
  auto optFound = store.reader(rtxn).get(id);
  REQUIRE(optFound.has_value());
  auto& found = *optFound;
  REQUIRE(found.tracks().size() == 10);
}

TEST_CASE("ListStore - delete", "[core][list]")
{
  auto temp = TempDir{};
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  auto wtxn = WriteTransaction{env};
  auto store = ListStore{Database{wtxn, "lists"}};
  wtxn.commit();

  // Create a list
  auto header = ListHeader{};

  auto data = std::vector<std::byte>(sizeof(ListHeader));
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
