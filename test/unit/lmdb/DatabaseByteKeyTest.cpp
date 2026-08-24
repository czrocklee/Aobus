// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/TestFixtureSupport.h"
#include "test/unit/lmdb/LmdbTestSupport.h"
#include <ao/lmdb/Database.h>
#include <ao/lmdb/Environment.h>
#include <ao/utility/ByteView.h>

#include <catch2/catch_test_macros.hpp>

#include <optional>

namespace ao::lmdb::test
{
  TEST_CASE("ByteKeyDatabase - supports reader and writer operations", "[lmdb][unit][database][byte-key]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = kEnvNoTls, .maxDatabases = 20});

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

  TEST_CASE("ByteKeyDatabase::Reader - lowerBound seeks to the first key not less than its target",
            "[lmdb][unit][database][byte-key]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = kEnvNoTls, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto db = openByteKeyDatabase(wtxn, "byte_database");
    auto writer = db.writer(wtxn);
    REQUIRE(writer.create(createStringData("alpha"), createStringData("one")));
    REQUIRE(writer.create(createStringData("delta"), createStringData("two")));
    REQUIRE(writer.create(createStringData("omega"), createStringData("three")));
    REQUIRE(wtxn.commit());

    auto const rtxn = beginReadTransaction(env);
    auto const reader = db.reader(rtxn);

    SECTION("exact target")
    {
      auto const target = createStringData("delta");
      auto const it = reader.lowerBound(target);
      REQUIRE(it != reader.end());
      CHECK(utility::bytes::stringView(it->first) == "delta");
      CHECK(utility::bytes::stringView(it->second) == "two");
    }

    SECTION("target between stored keys")
    {
      auto const target = createStringData("echo");
      auto const it = reader.lowerBound(target);
      REQUIRE(it != reader.end());
      CHECK(utility::bytes::stringView(it->first) == "omega");
      CHECK(utility::bytes::stringView(it->second) == "three");
    }

    SECTION("target after the final key")
    {
      auto const target = createStringData("zulu");
      CHECK(reader.lowerBound(target) == reader.end());
    }
  }

  TEST_CASE("ByteKeyDatabase::Reader::Iterator - destruction remains safe after a read transaction ends",
            "[lmdb][regression][cursor-lifetime]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = kEnvNoTls, .maxDatabases = 20});

    auto seedTransaction = beginWriteTransaction(env);
    auto db = openByteKeyDatabase(seedTransaction, "byte_database");
    REQUIRE(db.writer(seedTransaction).create(createStringData("key"), createStringData("value")));
    REQUIRE(seedTransaction.commit());

    auto iterator = ByteKeyDatabase::Reader::Iterator{};

    {
      auto transaction = beginReadTransaction(env);
      iterator = db.reader(transaction).begin();
      REQUIRE(iterator != ByteKeyDatabase::Reader::Iterator{});
      CHECK(utility::bytes::stringView(iterator->first) == "key");
    }

    iterator = {};
    CHECK(iterator == ByteKeyDatabase::Reader::Iterator{});
  }

  TEST_CASE("ByteKeyDatabase::Reader::Iterator - destruction remains safe after a write transaction ends",
            "[lmdb][regression][cursor-lifetime]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = kEnvNoTls, .maxDatabases = 20});

    auto createTransaction = beginWriteTransaction(env);
    auto db = openByteKeyDatabase(createTransaction, "byte_database");
    REQUIRE(db.writer(createTransaction).create(createStringData("key"), createStringData("value")));
    REQUIRE(createTransaction.commit());

    auto iterator = ByteKeyDatabase::Reader::Iterator{};

    {
      auto transaction = beginWriteTransaction(env);
      iterator = db.reader(transaction).begin();
      REQUIRE(iterator != ByteKeyDatabase::Reader::Iterator{});
      CHECK(utility::bytes::stringView(iterator->first) == "key");
      REQUIRE(transaction.commit());
    }

    iterator = {};
    CHECK(iterator == ByteKeyDatabase::Reader::Iterator{});
  }

  TEST_CASE("ByteKeyDatabase::Writer - destruction remains safe after its transaction object is gone",
            "[lmdb][regression][cursor-lifetime]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = kEnvNoTls, .maxDatabases = 20});

    auto createTransaction = beginWriteTransaction(env);
    auto db = openByteKeyDatabase(createTransaction, "byte_database");
    REQUIRE(createTransaction.commit());

    auto optWriter = std::optional<ByteKeyDatabase::Writer>{};

    {
      auto transaction = beginWriteTransaction(env);
      optWriter.emplace(db.writer(transaction));
      REQUIRE(optWriter->create(createStringData("key"), createStringData("persisted")));
      REQUIRE(transaction.commit());
    }

    optWriter.reset();

    auto const transaction = beginReadTransaction(env);
    auto const optData = db.reader(transaction).get(createStringData("key"));
    REQUIRE(optData);
    CHECK(utility::bytes::stringView(*optData) == "persisted");
  }
} // namespace ao::lmdb::test
