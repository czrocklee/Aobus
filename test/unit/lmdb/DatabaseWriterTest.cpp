// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "test/unit/TestUtils.h"
#include "test/unit/lmdb/LmdbTestSupport.h"
#include <ao/Error.h>
#include <ao/Exception.h>
#include <ao/lmdb/Database.h>
#include <ao/lmdb/Environment.h>
#include <ao/utility/ByteView.h>

#include <catch2/catch_test_macros.hpp>
#include <lmdb.h>

#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <utility>

namespace ao::lmdb::test
{
  TEST_CASE("Database::Writer - create with id and data", "[lmdb][unit][database][writer]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto db = openDatabase(wtxn, "test");
    auto writer = db.writer(wtxn);
    REQUIRE(writer.create(1, createStringData("hello")));
    REQUIRE(wtxn.commit());

    // Verify via reader
    auto const rtxn = beginReadTransaction(env);
    auto const reader = db.reader(rtxn);
    auto const optData = reader.get(1);
    REQUIRE(optData);
    REQUIRE(utility::bytes::stringView(*optData) == "hello");
  }

  TEST_CASE("Database::Writer - create with id and size", "[lmdb][unit][database][writer]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto db = openDatabase(wtxn, "test");
    auto writer = db.writer(wtxn);
    auto const result = writer.create(1, 10);
    REQUIRE(result);
    REQUIRE(!result->empty());
    REQUIRE(result->size() == 10);

    // Write data into the reserved space
    std::memset(result->data(), 'x', 10);

    REQUIRE(wtxn.commit());

    // Verify via reader
    auto const rtxn = beginReadTransaction(env);
    auto const reader = db.reader(rtxn);
    auto const optData = reader.get(1);
    REQUIRE(optData);
    REQUIRE(optData->size() == 10);
    REQUIRE(utility::bytes::stringView(*optData) == std::string(10, 'x'));
  }

  TEST_CASE("Database::Writer - append with data", "[lmdb][unit][database][writer]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto db = openDatabase(wtxn, "test");
    auto writer = db.writer(wtxn);

    auto const id1 = writer.append(createStringData("first"));
    REQUIRE(id1);
    REQUIRE(*id1 == 1);

    auto const id2 = writer.append(createStringData("second"));
    REQUIRE(id2);
    REQUIRE(*id2 == 2);

    REQUIRE(wtxn.commit());

    // Verify via reader
    auto const rtxn = beginReadTransaction(env);
    auto const reader = db.reader(rtxn);
    auto const optData1 = reader.get(1);
    auto const optData2 = reader.get(2);
    REQUIRE(optData1);
    REQUIRE(optData2);
    CHECK(utility::bytes::stringView(*optData1) == "first");
    CHECK(utility::bytes::stringView(*optData2) == "second");
  }

  TEST_CASE("Database::Writer - append with size", "[lmdb][unit][database][writer]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto db = openDatabase(wtxn, "test");
    auto writer = db.writer(wtxn);

    auto appendResult1 = writer.append(8);
    REQUIRE(appendResult1);
    auto const& [id1, result1] = *appendResult1;
    CHECK(id1 == 1);
    REQUIRE(!result1.empty());
    REQUIRE(result1.size() == 8);
    std::memset(result1.data(), 'a', 8);

    auto appendResult2 = writer.append(12);
    REQUIRE(appendResult2);
    auto const& [id2, result2] = *appendResult2;
    CHECK(id2 == 2);
    REQUIRE(!result2.empty());
    REQUIRE(result2.size() == 12);
    std::memset(result2.data(), 'b', 12);

    REQUIRE(wtxn.commit());

    // Verify via reader
    auto const rtxn = beginReadTransaction(env);
    auto const reader = db.reader(rtxn);
    auto const optData1 = reader.get(1);
    auto const optData2 = reader.get(2);
    REQUIRE(optData1);
    REQUIRE(optData2);
    CHECK(utility::bytes::stringView(*optData1) == std::string(8, 'a'));
    CHECK(utility::bytes::stringView(*optData2) == std::string(12, 'b'));
  }

  TEST_CASE("Database::Writer - append reports exhausted integer key space", "[lmdb][unit][database][writer]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto db = openDatabase(wtxn, "test");
    auto writer = db.writer(wtxn);

    REQUIRE(writer.create(std::numeric_limits<std::uint32_t>::max(), createStringData("last")));
    REQUIRE(wtxn.commit());

    auto wtxn2 = beginWriteTransaction(env);
    auto writer2 = db.writer(wtxn2);

    auto const dataResult = writer2.append(createStringData("overflow"));
    REQUIRE(!dataResult);
    CHECK(dataResult.error().code == Error::Code::ResourceExhausted);

    auto const reserveResult = writer2.append(4);
    REQUIRE(!reserveResult);
    CHECK(reserveResult.error().code == Error::Code::ResourceExhausted);
  }

  TEST_CASE("Database::Writer - update existing record", "[lmdb][unit][database][writer]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto db = openDatabase(wtxn, "test");
    auto writer = db.writer(wtxn);

    // Create initial record
    REQUIRE(writer.create(1, createStringData("original")));
    REQUIRE(wtxn.commit());

    // Update the record
    auto wtxn2 = beginWriteTransaction(env);
    auto writer2 = db.writer(wtxn2);
    REQUIRE(writer2.update(1, createStringData("updated")));
    REQUIRE(wtxn2.commit());

    // Verify via reader
    auto const rtxn = beginReadTransaction(env);
    auto const reader = db.reader(rtxn);
    auto const optData = reader.get(1);
    REQUIRE(optData);
    CHECK(optData->size() == 7);
    CHECK(utility::bytes::stringView(*optData) == "updated");
  }

  TEST_CASE("Database::Writer - delete record", "[lmdb][unit][database][writer]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto db = openDatabase(wtxn, "test");
    auto writer = db.writer(wtxn);
    REQUIRE(writer.create(1, createStringData("test")));
    REQUIRE(wtxn.commit());

    // Verify exists
    {
      auto const rtxn = beginReadTransaction(env);
      auto const reader = db.reader(rtxn);
      auto const optData1 = reader.get(1);
      REQUIRE(optData1);
    }

    // Delete
    {
      auto deleteTransaction = beginWriteTransaction(env);
      auto deleteWriter = db.writer(deleteTransaction);
      REQUIRE(deleteWriter.del(1));
      REQUIRE(deleteTransaction.commit());
    }

    // Verify deleted
    {
      auto const rtxn = beginReadTransaction(env);
      auto const reader = db.reader(rtxn);
      auto const optData = reader.get(1);
      REQUIRE_FALSE(optData);
    }
  }

  TEST_CASE("Database::Writer - delete missing record returns false", "[lmdb][unit][database][writer]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto db = openDatabase(wtxn, "test");
    auto writer = db.writer(wtxn);

    REQUIRE_FALSE(writer.del(123));
  }

  TEST_CASE("Database::Writer - get within write transaction", "[lmdb][unit][database][writer]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto db = openDatabase(wtxn, "test");
    auto writer = db.writer(wtxn);

    REQUIRE(writer.create(42, createStringData("answer")));
    auto const optDataResult = writer.get(42);
    REQUIRE(optDataResult);
    CHECK(optDataResult->size() == 6);
    CHECK(utility::bytes::stringView(*optDataResult) == "answer");
  }

  TEST_CASE("Database::Writer - move constructor", "[lmdb][unit][database][writer]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto db = openDatabase(wtxn, "test");

    auto writer1 = db.writer(wtxn);
    REQUIRE(writer1.create(1, createStringData("test")));

    auto writer2 = Database::Writer{std::move(writer1)};
    // writer1 is now in moved-from state
    // writer2 should still be usable

    REQUIRE(wtxn.commit());
  }

  TEST_CASE("Database::Writer - create returns Conflict on duplicate id with data", "[lmdb][unit][database][writer]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto db = openDatabase(wtxn, "test");
    auto writer = db.writer(wtxn);

    REQUIRE(writer.create(1, createStringData("first")));

    auto const result = writer.create(1, createStringData("duplicate"));
    REQUIRE_FALSE(result);
    CHECK(result.error().code == Error::Code::Conflict);
    REQUIRE(wtxn.commit());
  }

  TEST_CASE("Database::Writer - create returns Conflict on duplicate id with size", "[lmdb][unit][database][writer]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto db = openDatabase(wtxn, "test");
    auto writer = db.writer(wtxn);

    REQUIRE(writer.create(1, 10));

    auto const result = writer.create(1, 5);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == Error::Code::Conflict);
    REQUIRE(wtxn.commit());
  }

  TEST_CASE("Database::Writer - throws when used after commit", "[lmdb][unit][database][writer]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto db = openDatabase(wtxn, "test");
    auto writer = db.writer(wtxn);
    REQUIRE(writer.create(1, createStringData("before")));
    REQUIRE(wtxn.commit());

    // After commit LMDB has closed the writer's cursor; every operation must
    // throw instead of dereferencing the dangling handle.
    REQUIRE_THROWS_AS(writer.create(2, createStringData("after")), Exception);
    REQUIRE_THROWS_AS(writer.update(1, createStringData("after")), Exception);
    REQUIRE_THROWS_AS(writer.del(1), Exception);
    REQUIRE_THROWS_AS(writer.get(1), Exception);
    REQUIRE_THROWS_AS(writer.clear(), Exception);
  }

  TEST_CASE("Database::Writer - construction rejects a finished transaction", "[lmdb][unit][database][writer]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});
    auto transaction = beginWriteTransaction(env);
    auto db = openDatabase(transaction, "test");
    REQUIRE(transaction.commit());

    CHECK_THROWS_AS(db.writer(transaction), Exception);
  }

  TEST_CASE("Database::Writer - move assignment releases a finished cursor without closing it",
            "[lmdb][unit][database][writer]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto firstTransaction = beginWriteTransaction(env);
    auto db = openDatabase(firstTransaction, "test");
    auto writer = db.writer(firstTransaction);
    REQUIRE(writer.create(1, createStringData("first")));
    REQUIRE(firstTransaction.commit());

    auto secondTransaction = beginWriteTransaction(env);
    auto replacement = db.writer(secondTransaction);
    writer = std::move(replacement);
    REQUIRE(writer.create(2, createStringData("second")));
    REQUIRE(secondTransaction.commit());
  }
} // namespace ao::lmdb::test
