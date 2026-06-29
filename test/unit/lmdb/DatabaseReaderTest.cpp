// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/lmdb/TestUtils.h"
#include <ao/lmdb/Database.h>
#include <ao/lmdb/Environment.h>
#include <ao/lmdb/Transaction.h>
#include <ao/utility/ByteView.h>

#include <catch2/catch_test_macros.hpp>
#include <lmdb.h>

#include <utility>

namespace ao::lmdb::test
{
  TEST_CASE("Database::Reader::Iterator - reaches end as normal state", "[lmdb][unit][database][reader]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto db = openDatabase(wtxn, "test");
    auto writer = db.writer(wtxn);
    REQUIRE(writer.create(1, createStringData("one")));
    REQUIRE(writer.create(2, createStringData("two")));
    REQUIRE(wtxn.commit());

    auto const rtxn = beginReadTransaction(env);
    auto const reader = db.reader(rtxn);

    auto it = reader.begin();
    REQUIRE(it != reader.end());
    REQUIRE(it->first == 1);

    REQUIRE_NOTHROW(++it);
    REQUIRE(it != reader.end());
    REQUIRE(it->first == 2);

    REQUIRE_NOTHROW(++it);
    CHECK(it == reader.end());
  }

  TEST_CASE("Database::Reader - iterates no records for empty databases", "[lmdb][unit][database][reader]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto db = openDatabase(wtxn, "test");
    REQUIRE(wtxn.commit());

    auto const rtxn = beginReadTransaction(env);
    auto const reader = db.reader(rtxn);
    CHECK(reader.begin() == reader.end());
  }

  TEST_CASE("Database::Reader - get returns records by integer key", "[lmdb][unit][database][reader]")
  {
    auto const temp = ao::test::TempDir{};
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
    CHECK(it == Database::Reader::Iterator{});
  }

  TEST_CASE("Database::Reader::Iterator - compares equal to itself", "[lmdb][unit][database][reader]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto db = openDatabase(wtxn, "test");
    REQUIRE(db.writer(wtxn).create(1, createStringData("data")));
    REQUIRE(wtxn.commit());

    auto const rtxn = beginReadTransaction(env);
    auto const reader = db.reader(rtxn);

    auto const it1 = reader.begin();
    auto const& it2 = it1;

    REQUIRE(it1 == it2);
  }

  TEST_CASE("Database::Reader::Iterator - move constructor", "[lmdb][unit][database][reader]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto db = openDatabase(wtxn, "test");
    REQUIRE(db.writer(wtxn).create(1, createStringData("data")));
    REQUIRE(wtxn.commit());

    auto const rtxn = beginReadTransaction(env);
    auto const reader = db.reader(rtxn);

    auto it1 = reader.begin();
    auto const it2 = Database::Reader::Iterator{std::move(it1)};
    REQUIRE(it2 != reader.end());
    CHECK(it2->first == 1);
    CHECK(utility::bytes::stringView(it2->second) == "data");
  }

  TEST_CASE("Database::Reader::Iterator - dereference", "[lmdb][unit][database][reader]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto db = openDatabase(wtxn, "test");
    auto writer = db.writer(wtxn);
    REQUIRE(writer.create(100, createStringData("value")));
    REQUIRE(wtxn.commit());

    auto const rtxn = beginReadTransaction(env);
    auto const reader = db.reader(rtxn);
    auto const it = reader.begin();
    CHECK(utility::bytes::stringView(it->second) == "value");
  }
} // namespace ao::lmdb::test
