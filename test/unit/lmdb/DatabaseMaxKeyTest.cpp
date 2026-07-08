// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/lmdb/TestUtils.h"
#include <ao/lmdb/Database.h>
#include <ao/lmdb/Environment.h>
#include <ao/lmdb/Transaction.h>

#include <catch2/catch_test_macros.hpp>
#include <lmdb.h>

namespace ao::lmdb::test
{
  TEST_CASE("Database::Reader - maxKey returns zero for empty databases", "[lmdb][unit][database-reader][max-key]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto db = openDatabase(wtxn, "test");
    REQUIRE(wtxn.commit());

    auto const rtxn = beginReadTransaction(env);
    auto const reader = db.reader(rtxn);
    CHECK(reader.maxKey() == 0);
  }

  TEST_CASE("Database::Reader - maxKey after append", "[lmdb][unit][database-reader][max-key]")
  {
    auto const temp = ao::test::TempDir{};
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
    CHECK(reader.maxKey() == *id2);
  }

  TEST_CASE("Database::Reader - maxKey tracks explicitly created keys", "[lmdb][unit][database-reader][max-key]")
  {
    auto const temp = ao::test::TempDir{};
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
    CHECK(reader.maxKey() == 10);
  }

  TEST_CASE("Database::Reader - maxKey after delete", "[lmdb][unit][database-reader][max-key]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto db = openDatabase(wtxn, "test");
    auto writer = db.writer(wtxn);

    REQUIRE(writer.create(1, createStringData("one")));
    REQUIRE(writer.create(2, createStringData("two")));
    REQUIRE(writer.create(3, createStringData("three")));
    REQUIRE(wtxn.commit());

    {
      auto wtxn2 = beginWriteTransaction(env);
      auto writer2 = db.writer(wtxn2);
      REQUIRE(writer2.del(3));
      REQUIRE(wtxn2.commit());
    }

    auto const rtxn = beginReadTransaction(env);
    auto const reader = db.reader(rtxn);
    CHECK(reader.maxKey() == 2);
  }

  TEST_CASE("Database::Reader - maxKey after deleting middle element", "[lmdb][unit][database-reader][max-key]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto db = openDatabase(wtxn, "test");
    auto writer = db.writer(wtxn);

    REQUIRE(writer.create(1, createStringData("one")));
    REQUIRE(writer.create(2, createStringData("two")));
    REQUIRE(writer.create(3, createStringData("three")));
    REQUIRE(wtxn.commit());

    {
      auto wtxn2 = beginWriteTransaction(env);
      auto writer2 = db.writer(wtxn2);
      REQUIRE(writer2.del(2));
      REQUIRE(wtxn2.commit());
    }

    auto const rtxn = beginReadTransaction(env);
    auto const reader = db.reader(rtxn);
    CHECK(reader.maxKey() == 3);
  }
} // namespace ao::lmdb::test
