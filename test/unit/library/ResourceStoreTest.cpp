// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators_all.hpp>
#include <catch2/matchers/catch_matchers_all.hpp>

#include <ao/library/ResourceStore.h>
#include <ao/lmdb/Database.h>
#include <ao/lmdb/Environment.h>
#include <ao/lmdb/Transaction.h>
#include <test/unit/lmdb/TestUtils.h>

namespace ao::library::test
{
  using namespace ao::lmdb;
  using namespace ao::lmdb::test;

  TEST_CASE("ResourceStore - create and read", "[core][resource]")
  {
    auto const temp = TempDir{};
    auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

    auto wtxn = WriteTransaction{env};
    auto store = ResourceStore{Database{wtxn, "resources"}};
    wtxn.commit();

    // Create a resource
    auto const data = createStringData("hello");
    auto const& buffer = data;

    auto wtxn2 = WriteTransaction{env};
    auto const id = store.writer(wtxn2).create(buffer);
    REQUIRE(id > 0);
    wtxn2.commit();

    // Read the resource
    auto rtxn = ReadTransaction{env};
    auto reader = store.reader(rtxn);
    auto it = reader.begin();
    REQUIRE(it != reader.end());
    REQUIRE(it->first == id);
  }

  TEST_CASE("ResourceStore - delete", "[core][resource]")
  {
    auto const temp = TempDir{};
    auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

    auto wtxn = WriteTransaction{env};
    auto store = ResourceStore{Database{wtxn, "resources"}};
    wtxn.commit();

    // Create a resource
    auto const data = createStringData("test");
    auto const& buffer = data;

    auto wtxn2 = WriteTransaction{env};
    auto const id = store.writer(wtxn2).create(buffer);
    wtxn2.commit();

    // Delete it
    auto wtxn3 = WriteTransaction{env};
    auto const deleted = store.writer(wtxn3).del(id);
    REQUIRE(deleted);
    wtxn3.commit();

    // Verify it's gone
    auto rtxn = ReadTransaction{env};
    auto reader = store.reader(rtxn);
    auto it = reader.begin();
    REQUIRE(it == reader.end());
  }

  TEST_CASE("ResourceStore - deduplication", "[core][resource]")
  {
    auto const temp = TempDir{};
    auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

    auto wtxn = WriteTransaction{env};
    auto store = ResourceStore{Database{wtxn, "resources"}};
    wtxn.commit();

    // Create first resource
    auto const data = createStringData("samedata");
    auto const& buffer = data;

    auto wtxn2 = WriteTransaction{env};
    auto const id1 = store.writer(wtxn2).create(buffer);
    wtxn2.commit();

    // Create same content again - should return same ID (deduplication)
    auto wtxn3 = WriteTransaction{env};
    auto const id2 = store.writer(wtxn3).create(buffer);
    REQUIRE(id2 == id1);
    wtxn3.commit();

    // Verify only one resource exists
    auto rtxn = ReadTransaction{env};
    auto reader = store.reader(rtxn);
    int count = 0;

    for (auto it = reader.begin(); it != reader.end(); ++it)
    {
      ++count;
    }

    REQUIRE(count == 1);
  }
} // namespace ao::library::test
