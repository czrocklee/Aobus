// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/TestFixtureSupport.h"
#include "test/unit/lmdb/LmdbTestSupport.h"
#include <ao/lmdb/Database.h>
#include <ao/lmdb/Environment.h>
#include <ao/utility/ByteView.h>

#include <catch2/catch_test_macros.hpp>
#include <lmdb.h>

namespace ao::lmdb::test
{
  TEST_CASE("ByteKeyDatabase - supports reader and writer operations", "[lmdb][unit][database][byte-key]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto db = openByteKeyDatabase(wtxn, "byte_database");
    auto writer = db.writer(wtxn);

    auto const key1 = createStringData("key1");
    auto const key2 = createStringData("another_key");
    auto const val1 = createStringData("value1");
    auto const val2 = createStringData("value2");

    REQUIRE(writer.create(key1, val1));
    REQUIRE(writer.create(key2, val2));
    REQUIRE(wtxn.commit());

    auto const rtxn = beginReadTransaction(env);
    auto const reader = db.reader(rtxn);

    SECTION("get works with byte keys")
    {
      auto const optRes1 = reader.get(key1);
      REQUIRE(optRes1);
      REQUIRE(utility::bytes::stringView(*optRes1) == "value1");

      auto const optRes2 = reader.get(key2);
      REQUIRE(optRes2);
      REQUIRE(utility::bytes::stringView(*optRes2) == "value2");
    }

    SECTION("iteration follows byte-key order")
    {
      auto it = reader.begin();
      REQUIRE(it != reader.end());
      // LMDB sorts lexicographically
      CHECK(utility::bytes::stringView(it->first) == "another_key");
      CHECK(utility::bytes::stringView(it->second) == "value2");

      ++it;
      REQUIRE(it != reader.end());
      CHECK(utility::bytes::stringView(it->first) == "key1");
      CHECK(utility::bytes::stringView(it->second) == "value1");

      ++it;
      REQUIRE(it == reader.end());
    }

    SECTION("Writer::get works with byte keys")
    {
      auto wtxn2 = beginWriteTransaction(env);
      auto writer2 = db.writer(wtxn2);
      auto const optRes = writer2.get(key1);
      REQUIRE(optRes);
      REQUIRE(utility::bytes::stringView(*optRes) == "value1");
    }

    SECTION("Writer::del works with byte keys")
    {
      auto wtxn2 = beginWriteTransaction(env);
      auto writer2 = db.writer(wtxn2);
      REQUIRE(writer2.del(key1));
      REQUIRE_FALSE(writer2.get(key1).has_value());
      REQUIRE(wtxn2.commit());
    }
  }
} // namespace ao::lmdb::test
