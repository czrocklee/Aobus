// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/lmdb/Environment.h>

#include "test/unit/TestFixtureSupport.h"
#include "test/unit/lmdb/LmdbTestSupport.h"
#include <ao/Error.h>
#include <ao/lmdb/Transaction.h>

#include <catch2/catch_test_macros.hpp>
#include <lmdb.h>

#include <type_traits>
#include <utility>

namespace ao::lmdb::test
{
  namespace
  {
    template<typename T>
    concept HasPublicEnvironmentHandle = requires(T const& environment) { environment.handle(); };
  } // namespace

  TEST_CASE("Environment - openEnvironment creates usable environments", "[lmdb][unit][environment]")
  {
    auto temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    // Verify by starting a transaction
    auto txn = beginWriteTransaction(env);
  }

  TEST_CASE("Environment - open returns IoError for missing directory", "[lmdb][unit][environment]")
  {
    auto temp = ao::test::TempDir{};
    auto const result =
      Environment::open((temp.path() / "missing").string(), {.flags = MDB_CREATE, .maxDatabases = 20});

    REQUIRE_FALSE(result);
    CHECK(result.error().code == Error::Code::IoError);
  }

  TEST_CASE("Environment - open returns environment on success", "[lmdb][unit][environment]")
  {
    auto temp = ao::test::TempDir{};
    auto envRes = Environment::open(temp.path().string(), {.flags = MDB_CREATE, .maxDatabases = 20});
    CHECK(envRes);

    auto txnRes = WriteTransaction::begin(*envRes);
    CHECK(txnRes);
  }

  TEST_CASE("Environment - move constructor transfers ownership", "[lmdb][unit][environment]")
  {
    auto const temp = ao::test::TempDir{};
    auto env1 = openEnvironment(temp.path(), {.flags = MDB_NOTLS, .maxDatabases = 20});
    auto env2 = Environment{std::move(env1)};
    auto transaction = beginReadTransaction(env2);
    CHECK(transaction.isActive());
  }

  TEST_CASE("Environment - move assignment transfers ownership", "[lmdb][unit][environment]")
  {
    auto const firstTemp = ao::test::TempDir{};
    auto const secondTemp = ao::test::TempDir{};
    auto env1 = openEnvironment(firstTemp.path(), {.flags = MDB_NOTLS, .maxDatabases = 20});
    auto env2 = openEnvironment(secondTemp.path(), {.flags = MDB_NOTLS, .maxDatabases = 20});

    env2 = std::move(env1);
    auto transaction = beginReadTransaction(env2);
    CHECK(transaction.isActive());
  }

  TEST_CASE("Environment - exposes move-only ownership and a private native handle", "[lmdb][unit][environment]")
  {
    STATIC_REQUIRE(std::is_move_constructible_v<Environment>);
    STATIC_REQUIRE(std::is_move_assignable_v<Environment>);
    STATIC_REQUIRE_FALSE(std::is_copy_constructible_v<Environment>);
    STATIC_REQUIRE_FALSE(std::is_copy_assignable_v<Environment>);
    STATIC_REQUIRE_FALSE(HasPublicEnvironmentHandle<Environment>);
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
