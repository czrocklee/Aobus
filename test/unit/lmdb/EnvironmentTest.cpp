// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators_all.hpp>
#include <catch2/matchers/catch_matchers_all.hpp>

#include <ao/lmdb/Database.h>
#include <ao/lmdb/Environment.h>
#include <ao/lmdb/Transaction.h>
#include <lmdb.h>
#include <test/unit/lmdb/TestUtils.h>

#include <string_view>

using namespace ao::lmdb;

// ============================================================================
// Environment Tests
// ============================================================================

TEST_CASE("Environment - create", "[lmdb][environment]")
{
  auto temp = TempDir{};
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  // Verify by starting a transaction
  auto txn = WriteTransaction{env};
}

TEST_CASE("Environment - move constructor", "[lmdb][environment]")
{
  auto path = std::filesystem::temp_directory_path() / "rs_lmdb_move_test";
  std::filesystem::create_directory(path);

  auto env1 = Environment{path.string(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  auto env2 = Environment{std::move(env1)};
  // env1 is now in moved-from state
  // env2 should own the environment

  std::filesystem::remove_all(path);
}

TEST_CASE("Environment - move assignment", "[lmdb][environment]")
{
  auto path = std::filesystem::temp_directory_path() / "rs_lmdb_move_assign_test";
  std::filesystem::create_directory(path);

  auto env1 = Environment{path.string(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  auto env2 = Environment{path.string(), {.flags = MDB_CREATE, .maxDatabases = 20}};
  env2 = std::move(env1);

  std::filesystem::remove_all(path);
}

TEST_CASE("Environment - constructor with path", "[lmdb][environment]")
{
  auto temp = TempDir{};
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  // Verify we can create a transaction
  auto txn = ReadTransaction{env};
  auto wtxn = WriteTransaction{env};
}
