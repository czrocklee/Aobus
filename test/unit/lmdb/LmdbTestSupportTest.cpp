// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/unit/lmdb/LmdbTestSupport.h"

#include "test/unit/TestFixtureSupport.h"
#include <ao/lmdb/Database.h>
#include <ao/utility/Path.h>
#include <ao/utility/Raii.h>

#include <catch2/catch_test_macros.hpp>
#include <lmdb.h>

#include <array>
#include <cstddef>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <vector>

namespace ao::lmdb::test
{
  TEST_CASE("LmdbTestSupport - existing record probes own bytes and leave the data file unchanged",
            "[lmdb][unit][test-support]")
  {
    auto const temp = ao::test::TempDir{};
    auto const expected = createTestData(256);
    auto const path = temp.path() / std::filesystem::path{u8"整数-音楽"};
    REQUIRE(std::filesystem::create_directory(path));

    {
      auto environment = openEnvironment(path, {.maxDatabases = 3});
      auto transaction = beginWriteTransaction(environment);
      auto database = openIntegerKeyDatabase(transaction, "records");
      REQUIRE(database.writer(transaction).create(1, expected));
      REQUIRE(database.writer(transaction).create(2, std::span<std::byte const>{}));
      REQUIRE(database.writer(transaction).create(256, createStringData("native integer")));
      REQUIRE(IntegerKeyDatabase::open(transaction, "other"));
      auto mainRes = ByteKeyDatabase::main(transaction);
      REQUIRE(mainRes);
      REQUIRE(mainRes->writer(transaction).create(createStringData("ordinary"), createStringData("catalog value")));
      REQUIRE(transaction.commit());
    }

    auto const dataPath = path / "data.mdb";
    auto const before = ao::test::readFile(dataPath);
    REQUIRE_FALSE(before.empty());

    SECTION("the owning copy survives the probe environment and transaction")
    {
      auto const optBytes = readExistingIntegerKeyRecord(path, "records", 1);
      REQUIRE(optBytes);
      CHECK(*optBytes == expected);
    }

    SECTION("an empty stored value stays engaged")
    {
      auto const optBytes = readExistingIntegerKeyRecord(path, "records", 2);
      REQUIRE(optBytes);
      CHECK(optBytes->empty());
    }

    SECTION("a missing key is not an empty stored value")
    {
      CHECK_FALSE(readExistingIntegerKeyRecord(path, "records", 3));
    }

    SECTION("native four-byte keys are not lexical keys")
    {
      auto const optBytes = readExistingIntegerKeyRecord(path, "records", 256);
      REQUIRE(optBytes);
      CHECK(*optBytes == createStringData("native integer"));
    }

    SECTION("a missing database is a fixture error and is not created")
    {
      REQUIRE_THROWS_AS(readExistingIntegerKeyRecord(path, "missing", 1), std::runtime_error);
      REQUIRE_THROWS_AS(readExistingIntegerKeyRecord(path, "missing", 1), std::runtime_error);
    }

    SECTION("an ordinary catalog row is not a database or a missing record")
    {
      REQUIRE_THROWS_AS(readExistingIntegerKeyRecord(path, "ordinary", 1), std::runtime_error);
    }

    // Includes the catalog and payload; lock-file coordination is not payload mutation.
    CHECK(ao::test::readFile(dataPath) == before);
    // Reopening after every path also proves the helper released native owners.
    CHECK(readExistingIntegerKeyRecord(path, "records", 1) == expected);
  }

  TEST_CASE("LmdbTestSupport - readonly inspection rejects comparator and duplicate flags without repair",
            "[lmdb][unit][test-support]")
  {
    constexpr auto kFlags = std::to_array<unsigned int>({
      0,
      MDB_REVERSEKEY,
      MDB_INTEGERKEY | MDB_REVERSEKEY,
      MDB_INTEGERKEY | MDB_DUPSORT,
      MDB_INTEGERKEY | MDB_DUPSORT | MDB_DUPFIXED,
      MDB_INTEGERKEY | MDB_DUPSORT | MDB_DUPFIXED | MDB_INTEGERDUP,
      MDB_INTEGERKEY | MDB_DUPSORT | MDB_REVERSEDUP,
    });

    for (auto const flags : kFlags)
    {
      auto const temp = ao::test::TempDir{};

      {
        ::MDB_env* environment = nullptr;
        REQUIRE(::mdb_env_create(&environment) == MDB_SUCCESS);
        auto environmentPtr = utility::makeUniquePtr<::mdb_env_close>(environment);
        REQUIRE(::mdb_env_set_maxdbs(environment, 1) == MDB_SUCCESS);
        auto const utf8Path = utility::pathToUtf8(temp.path());
        REQUIRE(::mdb_env_open(environment, utf8Path.c_str(), 0, 0644) == MDB_SUCCESS);
        ::MDB_txn* transaction = nullptr;
        REQUIRE(::mdb_txn_begin(environment, nullptr, 0, &transaction) == MDB_SUCCESS);
        auto transactionPtr = utility::makeUniquePtr<::mdb_txn_abort>(transaction);
        ::MDB_dbi database = 0;
        REQUIRE(::mdb_dbi_open(transaction, "records", MDB_CREATE | flags, &database) == MDB_SUCCESS);
        REQUIRE(::mdb_txn_commit(transactionPtr.release()) == MDB_SUCCESS);
      }

      auto const dataPath = temp.path() / "data.mdb";
      auto const before = ao::test::readFile(dataPath);
      REQUIRE_FALSE(before.empty());
      REQUIRE_THROWS_AS(readExistingIntegerKeyRecord(temp.path(), "records", 1), std::runtime_error);
      CHECK(ao::test::readFile(dataPath) == before);
      REQUIRE_THROWS_AS(readExistingIntegerKeyRecord(temp.path(), "records", 1), std::runtime_error);
      CHECK(ao::test::readFile(dataPath) == before);
    }
  }
} // namespace ao::lmdb::test
