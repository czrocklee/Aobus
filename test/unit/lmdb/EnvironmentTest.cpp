// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "test/unit/lmdb/TestUtils.h"
#include <ao/lmdb/Environment.h>
#include <ao/lmdb/Transaction.h>

#include <catch2/catch_test_macros.hpp>
#include <lmdb.h>

#include <filesystem>
#include <utility>

namespace ao::lmdb::test
{
  TEST_CASE("Environment - create", "[lmdb][unit][environment]")
  {
    auto temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    // Verify by starting a transaction
    auto txn = beginWriteTransaction(env);
  }

  TEST_CASE("Environment - open returns IoError for missing directory", "[lmdb][unit][environment]")
  {
    auto temp = ao::test::TempDir{};
    auto const result = Environment::open(temp.path() / "missing", {.flags = MDB_CREATE, .maxDatabases = 20});

    REQUIRE_FALSE(result);
    CHECK(result.error().code == Error::Code::IoError);
  }

  TEST_CASE("Environment - open returns environment on success", "[lmdb][unit][environment]")
  {
    auto temp = ao::test::TempDir{};
    auto env = Environment::open(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});
    REQUIRE(env);

    auto txn = WriteTransaction::begin(*env);
    REQUIRE(txn);
  }

  TEST_CASE("Environment - move constructor", "[lmdb][unit][environment]")
  {
    auto path = std::filesystem::temp_directory_path() / "rs_lmdb_move_test";
    std::filesystem::create_directory(path);

    auto env1 = openEnvironment(path.string(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto env2 = Environment{std::move(env1)};
    // env1 is now in moved-from state
    // env2 should own the environment

    std::filesystem::remove_all(path);
  }

  TEST_CASE("Environment - move assignment", "[lmdb][unit][environment]")
  {
    auto path = std::filesystem::temp_directory_path() / "rs_lmdb_move_assign_test";
    std::filesystem::create_directory(path);

    auto env1 = openEnvironment(path.string(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto env2 = openEnvironment(path.string(), {.flags = MDB_CREATE, .maxDatabases = 20});
    env2 = std::move(env1);

    std::filesystem::remove_all(path);
  }

  TEST_CASE("Environment - helper opens path", "[lmdb][unit][environment]")
  {
    auto temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    // Verify we can create transactions.
    auto txn = beginReadTransaction(env);
    auto wtxn = beginWriteTransaction(env);
  }

  TEST_CASE("Environment - helper uses default options", "[lmdb][unit][environment]")
  {
    auto temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path());

    // Verify it opened correctly by starting a transaction.
    auto rtxn = beginReadTransaction(env);
  }
} // namespace ao::lmdb::test
