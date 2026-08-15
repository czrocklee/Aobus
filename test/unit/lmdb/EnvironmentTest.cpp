// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/lmdb/Environment.h>

#include "test/unit/TestFixtureSupport.h"
#include "test/unit/lmdb/LmdbTestSupport.h"
#include <ao/Error.h>
#include <ao/lmdb/Database.h>
#include <ao/lmdb/Transaction.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace ao::lmdb::test
{
  namespace
  {
    template<typename T>
    concept HasPublicEnvironmentHandle = requires(T const& environment) { environment.handle(); };

    // Small enough that filling past half of one is quick, large enough that a
    // doubling is unambiguous against LMDB's own 1 MiB starting size.
    constexpr auto kFloor = std::uint64_t{16} * 1024 * 1024;
    constexpr auto kCeiling = std::uint64_t{1024} * 1024 * 1024;
    constexpr auto kWideMap = std::size_t{64} * 1024 * 1024;
    constexpr auto kFillRecordSize = std::size_t{4096};
    constexpr auto kFillBatchSize = std::uint32_t{64};
    // Every batch adds pages, so a bound this loose only catches a map that has
    // stopped growing at all.
    constexpr auto kFillBatchLimit = 4096;
    // Past half of the floor, which is where the growth rule starts asking for a
    // larger map.
    constexpr auto kPastHalfOfFloor = (kFloor / 2) + (kFloor / 8);

    Environment::Options managedOptions(std::uint64_t const floorBytes, std::uint64_t const ceilingBytes)
    {
      // Both floors carry the same figure so the expected capacity does not
      // depend on whether the filesystem under the temporary directory can hold
      // a hole, which decides which of the two the rule consults.
      return Environment::Options{
        .flags = kEnvNoTls,
        .maxDatabases = 4,
        .capacity = {.minimumMapBytes = floorBytes,
                     .denseMinimumMapBytes = floorBytes,
                     .maximumMapBytes = ceilingBytes},
      };
    }

    /// Commits batches until the environment's recorded peak reaches @p targetBytes.
    void fillPastHighWater(Environment& env, std::uint64_t const targetBytes)
    {
      auto const data = createTestData(kFillRecordSize);
      std::int32_t batches = 0;

      while (env.capacity().highWaterBytes < targetBytes)
      {
        REQUIRE(batches < kFillBatchLimit);
        ++batches;

        auto txn = beginWriteTransaction(env);
        auto database = openIntegerKeyDatabase(txn, "records");
        auto writer = database.writer(txn);

        for (std::uint32_t index = 0; index < kFillBatchSize; ++index)
        {
          REQUIRE(writer.append(data));
        }

        REQUIRE(txn.commit());
      }
    }
  } // namespace

  TEST_CASE("Environment - openEnvironment creates usable environments", "[lmdb][unit][environment]")
  {
    auto temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = kEnvNoTls, .maxDatabases = 20});

    // Verify by starting a transaction
    auto txn = beginWriteTransaction(env);
  }

  TEST_CASE("Environment - open returns IoError for missing directory", "[lmdb][unit][environment]")
  {
    auto temp = ao::test::TempDir{};
    auto const result = Environment::open(temp.path() / "missing", {.flags = kEnvNoTls, .maxDatabases = 20});

    REQUIRE_FALSE(result);
    CHECK(result.error().code == Error::Code::IoError);
  }

  TEST_CASE("Environment - open returns environment on success", "[lmdb][unit][environment]")
  {
    auto temp = ao::test::TempDir{};
    auto envRes = Environment::open(temp.path(), {.flags = kEnvNoTls, .maxDatabases = 20});
    CHECK(envRes);

    auto txnRes = WriteTransaction::begin(*envRes);
    CHECK(txnRes);
  }

  TEST_CASE("Environment - move constructor transfers ownership", "[lmdb][unit][environment]")
  {
    auto const temp = ao::test::TempDir{};
    auto env1 = openEnvironment(temp.path(), {.flags = kEnvNoTls, .maxDatabases = 20});
    auto env2 = Environment{std::move(env1)};
    auto transaction = beginReadTransaction(env2);
    CHECK(transaction.isActive());
  }

  TEST_CASE("Environment - move assignment transfers ownership", "[lmdb][unit][environment]")
  {
    auto const firstTemp = ao::test::TempDir{};
    auto const secondTemp = ao::test::TempDir{};
    auto env1 = openEnvironment(firstTemp.path(), {.flags = kEnvNoTls, .maxDatabases = 20});
    auto env2 = openEnvironment(secondTemp.path(), {.flags = kEnvNoTls, .maxDatabases = 20});

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
    auto env = openEnvironment(temp.path(), {.flags = kEnvNoTls, .maxDatabases = 20});

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

  TEST_CASE("Environment - capacity reports the configured map and a growing high water",
            "[lmdb][unit][environment][capacity]")
  {
    constexpr std::size_t kMapSize = std::size_t{64} * 1024 * 1024;
    auto temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = kEnvNoTls, .maxDatabases = 4, .pinnedMapBytes = kMapSize});

    auto const empty = env.capacity();
    CHECK(empty.mapBytes == kMapSize);
    CHECK(empty.pageBytes > 0);
    CHECK(empty.highWaterBytes > 0);
    CHECK(empty.highWaterBytes < kMapSize);

    {
      auto const data = createTestData(4096);
      auto txn = beginWriteTransaction(env);
      auto database = openIntegerKeyDatabase(txn, "records");
      auto writer = database.writer(txn);

      for (std::uint32_t index = 0; index < 256; ++index)
      {
        REQUIRE(writer.append(data));
      }

      REQUIRE(txn.commit());
    }

    auto const filled = env.capacity();
    CHECK(filled.mapBytes == kMapSize);
    CHECK(filled.highWaterBytes > empty.highWaterBytes);
    CHECK(filled.highWaterBytes < kMapSize);
  }

  TEST_CASE("Environment - capacity high water is a peak rather than live data", "[lmdb][unit][environment][capacity]")
  {
    constexpr std::size_t kMapSize = std::size_t{64} * 1024 * 1024;
    auto temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = kEnvNoTls, .maxDatabases = 4, .pinnedMapBytes = kMapSize});

    {
      auto const data = createTestData(4096);
      auto txn = beginWriteTransaction(env);
      auto database = openIntegerKeyDatabase(txn, "records");
      auto writer = database.writer(txn);

      for (std::uint32_t index = 0; index < 256; ++index)
      {
        REQUIRE(writer.append(data));
      }

      REQUIRE(txn.commit());
    }

    auto const filled = env.capacity();

    {
      auto txn = beginWriteTransaction(env);
      auto database = openIntegerKeyDatabase(txn, "records");
      auto writer = database.writer(txn);
      REQUIRE(writer.clear());
      REQUIRE(txn.commit());
    }

    // Emptying the database returns its pages to the free list for reuse, which
    // never lowers the high water. A capacity decision has to read it that way.
    CHECK(env.capacity().highWaterBytes >= filled.highWaterBytes);
  }

  TEST_CASE("Environment - a managed map opens at the policy floor", "[lmdb][unit][environment][capacity]")
  {
    auto temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), managedOptions(kFloor, kCeiling));

    // Without a floor LMDB would start this database at its own 1 MiB default,
    // which the first real body of work passes.
    CHECK(env.capacity().mapBytes == kFloor);
  }

  TEST_CASE("Environment - a map larger than the floor survives reopening", "[lmdb][unit][environment][capacity]")
  {
    auto temp = ao::test::TempDir{};

    {
      auto env = openEnvironment(temp.path(), {.flags = kEnvNoTls, .maxDatabases = 4, .pinnedMapBytes = kWideMap});
      fillPastHighWater(env, kFloor / 4);
    }

    // Reopening under a policy whose floor is lower must not take back capacity
    // the database already recorded, or a session would find itself with less
    // room than the one before it had.
    auto env = openEnvironment(temp.path(), managedOptions(kFloor, kCeiling));
    CHECK(env.capacity().mapBytes == kWideMap);
  }

  TEST_CASE("Environment - a peak past half the map raises it at the next open", "[lmdb][unit][environment][capacity]")
  {
    auto temp = ao::test::TempDir{};

    {
      auto env = openEnvironment(temp.path(), managedOptions(kFloor, kCeiling));
      REQUIRE(env.capacity().mapBytes == kFloor);
      fillPastHighWater(env, kPastHalfOfFloor);
    }

    auto env = openEnvironment(temp.path(), managedOptions(kFloor, kCeiling));
    CHECK(env.capacity().mapBytes == kFloor * 2);
  }

  TEST_CASE("Environment - growth stops at the policy ceiling", "[lmdb][unit][environment][capacity]")
  {
    auto temp = ao::test::TempDir{};

    {
      auto env = openEnvironment(temp.path(), managedOptions(kFloor, kFloor));
      fillPastHighWater(env, kPastHalfOfFloor);
    }

    auto env = openEnvironment(temp.path(), managedOptions(kFloor, kFloor));
    CHECK(env.capacity().mapBytes == kFloor);
  }

  TEST_CASE("Environment - a pinned map size is never raised", "[lmdb][unit][environment][capacity]")
  {
    auto temp = ao::test::TempDir{};
    auto const pinnedOptions = Environment::Options{.flags = kEnvNoTls, .maxDatabases = 4, .pinnedMapBytes = kFloor};

    {
      auto env = openEnvironment(temp.path(), pinnedOptions);
      fillPastHighWater(env, kPastHalfOfFloor);
    }

    // A caller that asked for one exact capacity has to keep getting it, which is
    // what lets a test reach the end of a map on purpose.
    auto env = openEnvironment(temp.path(), pinnedOptions);
    CHECK(env.capacity().mapBytes == kFloor);
  }
} // namespace ao::lmdb::test
