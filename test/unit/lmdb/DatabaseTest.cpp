// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/lmdb/Database.h>

#include "lib/lmdb/detail/TransactionFailure.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/lmdb/LmdbTestSupport.h"
#include <ao/Error.h>
#include <ao/lmdb/Environment.h>

#include <catch2/catch_test_macros.hpp>
#include <lmdb.h>

#include <filesystem>
#include <memory>
#include <optional>
#include <utility>

namespace ao::lmdb::test
{
  namespace
  {
    template<typename Database, typename Transaction>
    concept SupportsDatabaseOpen = requires(Transaction& transaction) { Database::open(transaction, "database"); };

    void createIntegerKeyMain(std::filesystem::path const& path)
    {
      MDB_env* rawEnvironment = nullptr;
      REQUIRE(::mdb_env_create(&rawEnvironment) == MDB_SUCCESS);
      auto environmentPtr = std::unique_ptr<MDB_env, decltype(&::mdb_env_close)>{rawEnvironment, &::mdb_env_close};
      REQUIRE(::mdb_env_open(environmentPtr.get(), path.string().c_str(), 0, 0644) == MDB_SUCCESS);

      MDB_txn* rawTransaction = nullptr;
      REQUIRE(::mdb_txn_begin(environmentPtr.get(), nullptr, 0, &rawTransaction) == MDB_SUCCESS);
      auto transactionPtr = std::unique_ptr<MDB_txn, decltype(&::mdb_txn_abort)>{rawTransaction, &::mdb_txn_abort};
      MDB_dbi database = 0;
      REQUIRE(::mdb_dbi_open(transactionPtr.get(), nullptr, MDB_INTEGERKEY, &database) == MDB_SUCCESS);
      REQUIRE(::mdb_txn_commit(transactionPtr.release()) == MDB_SUCCESS);
    }
  } // namespace

  TEST_CASE("Database - helper opens database", "[lmdb][unit][database]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto db = openIntegerKeyDatabase(wtxn, "newdb");
    REQUIRE(wtxn.commit());

    auto const txn = beginReadTransaction(env);
    auto const reader = db.reader(txn);
    CHECK(reader.begin() == reader.end());
  }

  TEST_CASE("Typed databases - open returns the declared key type", "[lmdb][unit][database]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});
    auto wtxn = beginWriteTransaction(env);

    auto integerRes = IntegerKeyDatabase::open(wtxn, "integer");
    auto byteRes = ByteKeyDatabase::open(wtxn, "byte");

    CHECK(integerRes);
    CHECK(byteRes);
    REQUIRE(wtxn.commit());
  }

  TEST_CASE("Typed databases - named database open requires a write transaction", "[lmdb][unit][database]")
  {
    STATIC_REQUIRE(SupportsDatabaseOpen<IntegerKeyDatabase, WriteTransaction>);
    STATIC_REQUIRE(SupportsDatabaseOpen<ByteKeyDatabase, WriteTransaction>);
    STATIC_REQUIRE_FALSE(SupportsDatabaseOpen<IntegerKeyDatabase, ReadTransaction>);
    STATIC_REQUIRE_FALSE(SupportsDatabaseOpen<ByteKeyDatabase, ReadTransaction>);
  }

  TEST_CASE("Typed databases - openExisting never creates a missing named database", "[lmdb][unit][database]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});
    auto transaction = beginWriteTransaction(env);

    auto missingRes = IntegerKeyDatabase::openExisting(transaction, "missing");

    REQUIRE_FALSE(missingRes);
    CHECK(missingRes.error().code == Error::Code::NotFound);
    auto mainRes = ByteKeyDatabase::main(transaction);
    REQUIRE(mainRes);
    CHECK(mainRes->reader(transaction).entryCount() == 0);
    REQUIRE(transaction.commit());
  }

  TEST_CASE("Typed databases - openExisting validates exact native key flags", "[lmdb][unit][database]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});
    auto transaction = beginWriteTransaction(env);
    REQUIRE(IntegerKeyDatabase::open(transaction, "integer"));
    REQUIRE(ByteKeyDatabase::open(transaction, "byte"));

    auto integerRes = IntegerKeyDatabase::openExisting(transaction, "integer");
    auto byteRes = ByteKeyDatabase::openExisting(transaction, "byte");
    REQUIRE(integerRes);
    REQUIRE(byteRes);

    auto integerAsByteRes = ByteKeyDatabase::openExisting(transaction, "integer");
    auto byteAsIntegerRes = IntegerKeyDatabase::openExisting(transaction, "byte");
    REQUIRE_FALSE(integerAsByteRes);
    REQUIRE_FALSE(byteAsIntegerRes);
    CHECK(integerAsByteRes.error().code == Error::Code::CorruptData);
    CHECK(byteAsIntegerRes.error().code == Error::Code::CorruptData);
    CHECK(integerAsByteRes.error().message == "Named database 'integer' has flags 0x8 (expected 0x0)");
    CHECK(byteAsIntegerRes.error().message == "Named database 'byte' has flags 0x0 (expected 0x8)");
  }

  TEST_CASE("ByteKeyDatabase - main rejects integer-key storage", "[lmdb][regression][database]")
  {
    auto const temp = ao::test::TempDir{};
    createIntegerKeyMain(temp.path());

    auto env = openEnvironment(temp.path());
    auto transaction = beginWriteTransaction(env);
    auto mainRes = ByteKeyDatabase::main(transaction);

    REQUIRE_FALSE(mainRes);
    CHECK(mainRes.error().code == Error::Code::CorruptData);
    CHECK(mainRes.error().message == "Main database has flags 0x8 (expected 0x0)");
    CHECK(transaction.isActive());
  }

  TEST_CASE("Typed databases - create-capable open rejects an existing database with different key flags",
            "[lmdb][regression][database]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    {
      auto setup = beginWriteTransaction(env);
      REQUIRE(IntegerKeyDatabase::open(setup, "integer"));
      REQUIRE(ByteKeyDatabase::open(setup, "byte"));
      REQUIRE(setup.commit());
    }

    auto transaction = beginWriteTransaction(env);
    auto byteAsIntegerRes = IntegerKeyDatabase::open(transaction, "byte");
    auto integerAsByteRes = ByteKeyDatabase::open(transaction, "integer");

    REQUIRE_FALSE(byteAsIntegerRes);
    REQUIRE_FALSE(integerAsByteRes);
    CHECK(byteAsIntegerRes.error().code == Error::Code::CorruptData);
    CHECK(integerAsByteRes.error().code == Error::Code::CorruptData);
    CHECK(transaction.isActive());
  }

  TEST_CASE("Typed databases - openExisting rejects an ordinary main-database row", "[lmdb][unit][database]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});
    auto transaction = beginWriteTransaction(env);
    auto mainRes = ByteKeyDatabase::main(transaction);
    REQUIRE(mainRes);
    auto main = std::move(*mainRes);
    REQUIRE(main.writer(transaction).create(createStringData("ordinary"), createStringData("value")));

    auto ordinaryRes = ByteKeyDatabase::openExisting(transaction, "ordinary");

    REQUIRE_FALSE(ordinaryRes);
    CHECK(ordinaryRes.error().code == Error::Code::CorruptData);
  }

  TEST_CASE("Typed databases - failed write open unwinds and rolls back database creation",
            "[lmdb][regression][database]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 1});
    auto optFailure = std::optional<Error>{};

    try
    {
      auto transaction = beginWriteTransaction(env);
      REQUIRE(IntegerKeyDatabase::open(transaction, "first"));
      std::ignore = IntegerKeyDatabase::open(transaction, "second");
      FAIL("opening a database beyond maxdbs should throw");
    }
    catch (detail::TransactionFailure const& transactionFailure)
    {
      optFailure = transactionFailure.error();
    }

    REQUIRE(optFailure);
    CHECK(optFailure->code == Error::Code::IoError);

    auto verificationTransaction = beginWriteTransaction(env);
    auto firstRes = IntegerKeyDatabase::openExisting(verificationTransaction, "first");
    REQUIRE_FALSE(firstRes);
    CHECK(firstRes.error().code == Error::Code::NotFound);
    REQUIRE(verificationTransaction.commit());
  }
} // namespace ao::lmdb::test
