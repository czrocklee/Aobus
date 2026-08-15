// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/lmdb/Transaction.h>

#include "test/fatal/ProbeProcess.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/lmdb/LmdbTestSupport.h"
#include <ao/lmdb/Database.h>
#include <ao/lmdb/Environment.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace ao::lmdb::test
{
  namespace
  {
    template<typename Transaction>
    concept SupportsNestedBegin = requires(Transaction& transaction) { WriteTransaction::begin(transaction); };

    static_assert(!SupportsNestedBegin<WriteTransaction>);
  } // namespace

  TEST_CASE("ReadTransaction - helper starts transaction", "[lmdb][unit][transaction]")
  {
    auto temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = kEnvNoTls, .maxDatabases = 20});

    // First create database with write transaction
    auto wtxn = beginWriteTransaction(env);
    auto db = openIntegerKeyDatabase(wtxn, "test");
    REQUIRE(wtxn.commit());

    // Then use read transaction
    auto txn = beginReadTransaction(env);
    auto reader = db.reader(txn);
    CHECK(reader.begin() == reader.end()); // Empty DB
  }

  TEST_CASE("ReadTransaction - begin returns transaction", "[lmdb][unit][transaction]")
  {
    auto temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = kEnvNoTls, .maxDatabases = 20});

    auto txnRes = ReadTransaction::begin(env);

    CHECK(txnRes);
  }

  TEST_CASE("ReadTransaction - destructor aborts", "[lmdb][unit][transaction]")
  {
    auto temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = kEnvNoTls, .maxDatabases = 20});

    // Create database with write transaction
    auto wtxn = beginWriteTransaction(env);
    auto db = openIntegerKeyDatabase(wtxn, "test");
    REQUIRE(wtxn.commit());

    // Read transaction - destructor should abort
    {
      auto txn = beginReadTransaction(env);
      // Destructor should abort
    }

    // Should be able to start new transaction
    auto txn2 = beginReadTransaction(env);
    auto reader = db.reader(txn2);
    CHECK(reader.begin() == reader.end());
  }

  TEST_CASE("ReadTransaction - move constructor transfers usable transactions", "[lmdb][unit][transaction]")
  {
    auto temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = kEnvNoTls, .maxDatabases = 20});

    // Create database first with write transaction
    auto wtxn = beginWriteTransaction(env);
    auto db = openIntegerKeyDatabase(wtxn, "test");
    REQUIRE(wtxn.commit());

    auto txn1 = beginReadTransaction(env);
    auto txn2 = ReadTransaction{std::move(txn1)};
    // Verify moved transaction is valid by using it
    auto reader = db.reader(txn2);
    CHECK(reader.begin() == reader.end());
  }

  // ============================================================================
  // WriteTransaction Tests
  // ============================================================================
  TEST_CASE("WriteTransaction - helper starts transaction", "[lmdb][unit][transaction]")
  {
    auto temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = kEnvNoTls, .maxDatabases = 20});

    auto txn = beginWriteTransaction(env);
    // Verify transaction is valid by using it to create a database
    auto db = openIntegerKeyDatabase(txn, "test");
    [[maybe_unused]] auto writer = db.writer(txn);
  }

  TEST_CASE("WriteTransaction - begin returns transaction", "[lmdb][unit][transaction]")
  {
    auto temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = kEnvNoTls, .maxDatabases = 20});

    auto txnRes = WriteTransaction::begin(env);

    CHECK(txnRes);
  }

  TEST_CASE("WriteTransaction - commit persists written data", "[lmdb][unit][transaction]")
  {
    auto temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = kEnvNoTls, .maxDatabases = 20});

    // Create database, write data, commit
    auto wtxn = beginWriteTransaction(env);
    auto db = openIntegerKeyDatabase(wtxn, "test");
    auto writer = db.writer(wtxn);
    REQUIRE(writer.create(1, createStringData("test data")));
    CHECK(wtxn.isActive());
    CHECK_FALSE(wtxn.isFinished());
    REQUIRE(wtxn.commit());
    CHECK_FALSE(wtxn.isActive());
    CHECK(wtxn.isFinished());

    // Start a new transaction - should work now
    auto wtxn2 = beginWriteTransaction(env);
    auto reader = db.reader(wtxn2);
    auto it = reader.begin();
    REQUIRE(it != reader.end());
    REQUIRE(static_cast<std::uint32_t>(it->first) == 1);
  }

  TEST_CASE("WriteTransaction - destructor without commit aborts", "[lmdb][unit][transaction]")
  {
    auto temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = kEnvNoTls, .maxDatabases = 20});

    // Create database
    auto dbTxn = beginWriteTransaction(env);
    auto db = openIntegerKeyDatabase(dbTxn, "test");
    REQUIRE(dbTxn.commit());

    // Write data without committing - transaction should abort on destruction
    {
      auto txn = beginWriteTransaction(env);
      auto writer = db.writer(txn);
      REQUIRE(writer.create(1, createStringData("uncommitted")));
      // Without commit, transaction aborts on destruction
    }

    // Data should not be visible
    auto txn = beginReadTransaction(env);
    auto reader = db.reader(txn);
    REQUIRE_FALSE(reader.get(1).has_value());
  }

  TEST_CASE("WriteTransaction - explicit abort is terminal and idempotent", "[lmdb][unit][transaction]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = kEnvNoTls, .maxDatabases = 20});
    auto transaction = beginWriteTransaction(env);
    auto db = openIntegerKeyDatabase(transaction, "test");
    auto writer = db.writer(transaction);
    REQUIRE(writer.create(1, createStringData("aborted")));

    transaction.abort();
    CHECK_FALSE(transaction.isActive());
    CHECK(transaction.isFinished());
    CHECK_NOTHROW(transaction.abort());
  }

  TEST_CASE("WriteTransaction - move constructor transfers usable transactions", "[lmdb][unit][transaction]")
  {
    auto temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = kEnvNoTls, .maxDatabases = 20});

    // First create the database
    auto wtxn = beginWriteTransaction(env);
    auto db = openIntegerKeyDatabase(wtxn, "test");
    REQUIRE(wtxn.commit());

    // Now test move
    auto txn1 = beginWriteTransaction(env);
    auto txn2 = WriteTransaction{std::move(txn1)};
    // Verify moved transaction is valid by using it
    [[maybe_unused]] auto writer = db.writer(txn2);
    REQUIRE(txn2.commit());
  }

  TEST_CASE("WriteTransaction - database-open admission releases after every terminal path",
            "[lmdb][regression][transaction][concurrency]")
  {
    static constexpr auto kScenarios = std::array<std::string_view, 3>{
      "lmdb-database-open-admission-release-commit",
      "lmdb-database-open-admission-release-abort",
      "lmdb-database-open-admission-release-destruction",
    };
    constexpr auto kTimeout = std::chrono::seconds{15};
    auto const executablePath = ao::test::siblingProbeExecutablePath("ao_library_probe");
    REQUIRE_FALSE(executablePath.empty());

    for (auto const scenarioName : kScenarios)
    {
      INFO("probe: " << scenarioName);
      auto const scratch = ao::test::TempDir{};
      auto scenario = std::string{scenarioName};
      scenario.append(":").append(scratch.path().filename().string());
      auto const result = ao::test::runProbeProcess(executablePath, scenario, kTimeout);

      REQUIRE(result.started);
      CHECK(result.launchError.empty());
      CHECK_FALSE(result.timedOut);
      REQUIRE(result.hasSuccessfulExit());
      CHECK_FALSE(result.hasFatalTermination());
      CHECK(result.standardOutput == scenarioName);
      CHECK(result.standardError.empty());
    }
  }
} // namespace ao::lmdb::test
