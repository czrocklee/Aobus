// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "test/unit/lmdb/TestUtils.h"
#include <ao/Type.h>
#include <ao/library/ListLayout.h>
#include <ao/library/ListStore.h>
#include <ao/library/ListView.h>
#include <ao/lmdb/Database.h>
#include <ao/lmdb/Environment.h>
#include <ao/lmdb/Transaction.h>

#include <catch2/catch_test_macros.hpp>
#include <lmdb.h>

#include <cstddef>
#include <cstring>
#include <span>
#include <utility>
#include <vector>

namespace ao::library::test
{
  using namespace ao::lmdb;
  using namespace ao::lmdb::test;

  namespace
  {
    std::pair<ListId, ListView> requireCreate(ListStore::Writer writer, std::span<std::byte const> data)
    {
      auto result = writer.create(data);
      REQUIRE(result);
      return *result;
    }
  } // namespace

  TEST_CASE("ListStore - create and read", "[library][unit][list]")
  {
    auto const temp = TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto store = ListStore{openDatabase(wtxn, "lists")};
    wtxn.commit();

    // Create a list
    auto header = ListHeader{.trackIdsCount = 5, .nameOffset = 0};

    auto data = std::vector<std::byte>(sizeof(ListHeader));
    std::memcpy(data.data(), &header, sizeof(ListHeader));

    auto wtxn2 = beginWriteTransaction(env);
    auto const [id, view] = requireCreate(store.writer(wtxn2), data);
    wtxn2.commit();

    // Read the list
    auto rtxn = beginReadTransaction(env);
    auto reader = store.reader(rtxn);
    auto it = reader.begin();
    REQUIRE(it != reader.end());
    REQUIRE((*it).first == id);
  }

  TEST_CASE("ListStore - read by id", "[library][unit][list]")
  {
    auto const temp = TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto store = ListStore{openDatabase(wtxn, "lists")};
    wtxn.commit();

    // Create a list
    auto header = ListHeader{.trackIdsCount = 10, .nameOffset = 0};

    auto const trackIdsSize = static_cast<std::size_t>(header.trackIdsCount) * sizeof(TrackId);
    auto data = std::vector<std::byte>(sizeof(ListHeader) + trackIdsSize);
    std::memcpy(data.data(), &header, sizeof(ListHeader));

    auto wtxn2 = beginWriteTransaction(env);
    auto const [id, view] = requireCreate(store.writer(wtxn2), data);
    wtxn2.commit();

    // Read by ID
    auto rtxn = beginReadTransaction(env);
    auto const optFound = store.reader(rtxn).get(id);
    REQUIRE(optFound.has_value());
    REQUIRE(optFound->tracks().size() == 10);
  }

  TEST_CASE("ListStore - delete", "[library][unit][list]")
  {
    auto const temp = TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto store = ListStore{openDatabase(wtxn, "lists")};
    wtxn.commit();

    // Create a list
    auto header = ListHeader{};

    auto data = std::vector<std::byte>(sizeof(ListHeader));
    std::memcpy(data.data(), &header, sizeof(ListHeader));

    auto wtxn2 = beginWriteTransaction(env);
    auto const [id, view] = requireCreate(store.writer(wtxn2), data);
    wtxn2.commit();

    // Delete it
    auto wtxn3 = beginWriteTransaction(env);
    REQUIRE(store.writer(wtxn3).del(id));
    wtxn3.commit();

    // Verify it's gone
    auto rtxn = beginReadTransaction(env);
    auto reader = store.reader(rtxn);
    auto it = reader.begin();
    REQUIRE(it == reader.end());
  }
} // namespace ao::library::test
