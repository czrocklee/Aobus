// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/TestFixtureSupport.h"
#include "test/unit/lmdb/LmdbTestSupport.h"
#include <ao/lmdb/Database.h>
#include <ao/lmdb/Environment.h>
#include <ao/utility/ByteView.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

namespace ao::lmdb::test
{
  namespace
  {
    template<typename Reader>
    concept HasIntegerGet = requires(Reader const& reader) { reader.get(std::uint32_t{1}); };

    template<typename Reader>
    concept HasByteGet = requires(Reader const& reader) { reader.get(std::span<std::byte const>{}); };

    template<typename Reader>
    concept HasMaxKey = requires(Reader const& reader) { reader.maxKey(); };

    static_assert(HasIntegerGet<IntegerKeyDatabase::Reader>);
    static_assert(!HasByteGet<IntegerKeyDatabase::Reader>);
    static_assert(HasMaxKey<IntegerKeyDatabase::Reader>);
    static_assert(!HasIntegerGet<ByteKeyDatabase::Reader>);
    static_assert(HasByteGet<ByteKeyDatabase::Reader>);
    static_assert(!HasMaxKey<ByteKeyDatabase::Reader>);
  } // namespace

  TEST_CASE("IntegerKeyDatabase::Reader::Iterator - reaches end as normal state", "[lmdb][unit][database][reader]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = kEnvNoTls, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto db = openIntegerKeyDatabase(wtxn, "test");
    auto writer = db.writer(wtxn);
    REQUIRE(writer.create(1, createStringData("one")));
    REQUIRE(writer.create(2, createStringData("two")));
    REQUIRE(wtxn.commit());

    auto const rtxn = beginReadTransaction(env);
    auto const reader = db.reader(rtxn);

    auto it = reader.begin();
    REQUIRE(it != reader.end());
    REQUIRE(static_cast<std::uint32_t>(it->first) == 1);

    REQUIRE_NOTHROW(++it);
    REQUIRE(it != reader.end());
    REQUIRE(static_cast<std::uint32_t>(it->first) == 2);

    REQUIRE_NOTHROW(++it);
    CHECK(it == reader.end());
  }

  TEST_CASE("IntegerKeyDatabase::Reader - iterates no records for empty databases", "[lmdb][unit][database][reader]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = kEnvNoTls, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto db = openIntegerKeyDatabase(wtxn, "test");
    REQUIRE(wtxn.commit());

    auto const rtxn = beginReadTransaction(env);
    auto const reader = db.reader(rtxn);
    CHECK(reader.begin() == reader.end());
  }

  TEST_CASE("IntegerKeyDatabase::Reader - get returns records by integer key", "[lmdb][unit][database][reader]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = kEnvNoTls, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto db = openIntegerKeyDatabase(wtxn, "test");
    auto writer = db.writer(wtxn);
    REQUIRE(writer.create(42, createStringData("answer")));
    REQUIRE(wtxn.commit());

    auto const rtxn = beginReadTransaction(env);
    auto const reader = db.reader(rtxn);

    SECTION("Existing key returns data")
    {
      auto const optData = reader.get(42);
      REQUIRE(optData);
      REQUIRE(utility::bytes::stringView(*optData) == "answer");
    }

    SECTION("Missing key returns nullopt")
    {
      auto const optData = reader.get(999);
      REQUIRE_FALSE(optData);
    }
  }

  TEST_CASE("IntegerKeyDatabase::Reader - entryCount reports visible row cardinality",
            "[lmdb][unit][database-reader][entry-count]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = kEnvNoTls, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto db = openIntegerKeyDatabase(wtxn, "test");

    SECTION("empty database")
    {
      REQUIRE(wtxn.commit());

      auto const rtxn = beginReadTransaction(env);
      CHECK(db.reader(rtxn).entryCount() == 0);
    }

    SECTION("dense integer keys")
    {
      auto writer = db.writer(wtxn);
      REQUIRE(writer.create(1, createStringData("one")));
      REQUIRE(writer.create(2, createStringData("two")));
      REQUIRE(writer.create(3, createStringData("three")));
      REQUIRE(wtxn.commit());

      auto const rtxn = beginReadTransaction(env);
      auto const reader = db.reader(rtxn);
      CHECK(reader.entryCount() == 3);
      CHECK(reader.maxKey() == 3);
    }

    SECTION("sparse integer keys")
    {
      auto writer = db.writer(wtxn);
      REQUIRE(writer.create(1, createStringData("one")));
      REQUIRE(writer.create(100, createStringData("one hundred")));
      REQUIRE(writer.create(1000, createStringData("one thousand")));
      REQUIRE(wtxn.commit());

      auto const rtxn = beginReadTransaction(env);
      auto const reader = db.reader(rtxn);
      CHECK(reader.entryCount() == 3);
      CHECK(reader.maxKey() == 1000);
    }
  }

  TEST_CASE("IntegerKeyDatabase::Reader::Iterator - default constructor", "[lmdb][unit][database][reader]")
  {
    auto const it = IntegerKeyDatabase::Reader::Iterator{};
    CHECK(it == IntegerKeyDatabase::Reader::Iterator{});
  }

  TEST_CASE("IntegerKeyDatabase::Reader::Iterator - compares equal to itself", "[lmdb][unit][database][reader]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = kEnvNoTls, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto db = openIntegerKeyDatabase(wtxn, "test");
    REQUIRE(db.writer(wtxn).create(1, createStringData("data")));
    REQUIRE(wtxn.commit());

    auto const rtxn = beginReadTransaction(env);
    auto const reader = db.reader(rtxn);

    auto const it1 = reader.begin();
    auto const& it2 = it1;

    REQUIRE(it1 == it2);
  }

  TEST_CASE("IntegerKeyDatabase::Reader::Iterator - move constructor", "[lmdb][unit][database][reader]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = kEnvNoTls, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto db = openIntegerKeyDatabase(wtxn, "test");
    REQUIRE(db.writer(wtxn).create(1, createStringData("data")));
    REQUIRE(wtxn.commit());

    auto const rtxn = beginReadTransaction(env);
    auto const reader = db.reader(rtxn);

    auto it1 = reader.begin();
    auto const it2 = IntegerKeyDatabase::Reader::Iterator{std::move(it1)};
    REQUIRE(it2 != reader.end());
    CHECK(static_cast<std::uint32_t>(it2->first) == 1);
    CHECK(utility::bytes::stringView(it2->second) == "data");
  }

  TEST_CASE("IntegerKeyDatabase::Reader::Iterator - dereference", "[lmdb][unit][database][reader]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = kEnvNoTls, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto db = openIntegerKeyDatabase(wtxn, "test");
    auto writer = db.writer(wtxn);
    REQUIRE(writer.create(100, createStringData("value")));
    REQUIRE(wtxn.commit());

    auto const rtxn = beginReadTransaction(env);
    auto const reader = db.reader(rtxn);
    auto const it = reader.begin();
    CHECK(utility::bytes::stringView(it->second) == "value");
  }
} // namespace ao::lmdb::test
