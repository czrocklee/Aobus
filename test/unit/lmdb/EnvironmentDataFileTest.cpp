// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "lib/lmdb/detail/EnvironmentDataFile.h"

#include "test/unit/TestFixtureSupport.h"
#include "test/unit/lmdb/LmdbTestSupport.h"
#include <ao/Error.h>
#include <ao/lmdb/Database.h>
#include <ao/lmdb/Environment.h>
#include <ao/lmdb/Transaction.h>
#include <ao/utility/FileAllocation.h>
#include <ao/utility/Path.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <latch>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace ao::lmdb::test
{
  namespace
  {
    // Large enough that allocating all of it would be unmistakable, small enough
    // that a volume without sparse support wastes only this much before failing.
    constexpr auto kLargeMapSize = std::size_t{256} * 1024 * 1024;
    constexpr auto kAllocationCeiling = kLargeMapSize / 8;
    constexpr auto kRecordSize = std::size_t{4096};
    constexpr auto kRecordCount = std::uint32_t{256};

    Environment::Options largeMapOptions()
    {
      return Environment::Options{.flags = kEnvNoTls, .maxDatabases = 4, .pinnedMapBytes = kLargeMapSize};
    }

    void appendRecords(Environment& env, std::uint32_t const count)
    {
      auto const data = createTestData(kRecordSize);
      auto txn = beginWriteTransaction(env);
      auto database = openIntegerKeyDatabase(txn, "records");
      auto writer = database.writer(txn);

      for (std::uint32_t index = 0; index < count; ++index)
      {
        REQUIRE(writer.append(data));
      }

      REQUIRE(txn.commit());
    }

    void requireRecordsReadable(Environment& env, std::uint32_t const count)
    {
      auto txn = beginWriteTransaction(env);
      auto database = openIntegerKeyDatabase(txn, "records");
      auto writer = database.writer(txn);

      for (std::uint32_t id = 1; id <= count; ++id)
      {
        auto const optValue = writer.get(id);
        REQUIRE(optValue);
        REQUIRE(optValue->size() == kRecordSize);
      }
    }
  } // namespace

  // These expectations need a data file whose unwritten remainder costs nothing:
  // a hole on Windows, and on POSIX a file LMDB never extends in the first place.
  TEST_CASE("Environment - a configured map size does not become disk usage", "[lmdb][unit][environment]")
  {
    auto const temp = ao::test::TempDir{};
    auto const dataPath = temp.path() / "data.mdb";

    {
      auto env = openEnvironment(temp.path(), largeMapOptions());
      CHECK(env.mapAllocation() == MapAllocation::OnDemand);
      CHECK(utility::allocatedFileBytes(dataPath) < kAllocationCeiling);
      appendRecords(env, kRecordCount);
    }

    auto const allocated = utility::allocatedFileBytes(dataPath);
    CHECK(allocated >= kRecordSize * kRecordCount);
    CHECK(allocated < kAllocationCeiling);
  }

  TEST_CASE("Environment - reopening an environment keeps allocation proportional", "[lmdb][unit][environment]")
  {
    auto const temp = ao::test::TempDir{};
    auto const dataPath = temp.path() / "data.mdb";

    {
      auto env = openEnvironment(temp.path(), largeMapOptions());
      appendRecords(env, kRecordCount);
    }

    auto const afterFirstSession = utility::allocatedFileBytes(dataPath);

    {
      auto env = openEnvironment(temp.path(), largeMapOptions());
      appendRecords(env, kRecordCount);
    }

    auto const afterSecondSession = utility::allocatedFileBytes(dataPath);
    CHECK(afterSecondSession > afterFirstSession);
    CHECK(afterSecondSession < kAllocationCeiling);
  }

  TEST_CASE("Environment - opens over a data file that already exists", "[lmdb][unit][environment]")
  {
    auto const temp = ao::test::TempDir{};
    auto const dataPath = temp.path() / "data.mdb";

    {
      auto env = openEnvironment(temp.path(), largeMapOptions());
      appendRecords(env, kRecordCount);
    }

    // Preparation has to accept the file a previous session left behind, not only
    // one it creates itself, because every later open repeats it.
    REQUIRE(std::filesystem::exists(dataPath));
    REQUIRE(detail::prepareEnvironmentDataFile(temp.path(), detail::DataFileAccess::ReadWrite));

    auto env = openEnvironment(temp.path(), largeMapOptions());
    CHECK(env.mapAllocation() == MapAllocation::OnDemand);
    requireRecordsReadable(env, kRecordCount);
    CHECK(utility::allocatedFileBytes(dataPath) < kAllocationCeiling);
  }

  TEST_CASE("Environment - opens a path whose name is not ASCII", "[lmdb][unit][environment]")
  {
    // Escaped rather than literal bytes so the expectation does not depend on how
    // a compiler reads this source file. LMDB decodes the path as UTF-8 and the
    // adapter has to convert it the same way.
    auto const nonAsciiName = std::string{"\xe9\x9f\xb3\xe4\xb9\x90-caf\xc3\xa9"};
    auto const temp = ao::test::TempDir{};
    auto const directory = temp.path() / utility::pathFromUtf8(nonAsciiName);
    REQUIRE(std::filesystem::create_directory(directory));

    {
      auto envRes = Environment::open(directory, largeMapOptions());
      REQUIRE(envRes);
      appendRecords(*envRes, kRecordCount);
    }

    CHECK(std::filesystem::exists(directory / "data.mdb"));

    auto envRes = Environment::open(directory, largeMapOptions());
    REQUIRE(envRes);
    requireRecordsReadable(*envRes, kRecordCount);
  }

  TEST_CASE("EnvironmentDataFile - concurrent preparation of one file all succeeds",
            "[lmdb][unit][environment][concurrency]")
  {
    constexpr std::size_t kThreadCount = 8;
    auto const temp = ao::test::TempDir{};
    auto failures = std::atomic{std::size_t{0}};
    auto denseReports = std::atomic{std::size_t{0}};
    auto start = std::latch{static_cast<std::ptrdiff_t>(kThreadCount)};

    {
      auto threads = std::vector<std::jthread>{};
      threads.reserve(kThreadCount);

      for (std::size_t index = 0; index < kThreadCount; ++index)
      {
        threads.emplace_back(
          [&temp, &failures, &denseReports, &start]
          {
            // Every thread waits for the last one, so they contend for the same
            // create-and-mark sequence rather than arriving in turn.
            start.arrive_and_wait();
            auto const allocationRes =
              detail::prepareEnvironmentDataFile(temp.path(), detail::DataFileAccess::ReadWrite);

            if (!allocationRes)
            {
              failures.fetch_add(1, std::memory_order_relaxed);
            }
            else if (*allocationRes == MapAllocation::WholeMap)
            {
              denseReports.fetch_add(1, std::memory_order_relaxed);
            }
          });
      }
    }

    CHECK(failures.load(std::memory_order_relaxed) == 0);
    // Every thread saw the same filesystem, so they cannot disagree about it.
    auto const dense = denseReports.load(std::memory_order_relaxed);
    CHECK((dense == 0 || dense == kThreadCount));

    // The environment still opens normally afterwards, and the file the racing
    // callers left behind is still one LMDB can size to the configured map.
    auto const dataPath = temp.path() / "data.mdb";
    auto env = openEnvironment(temp.path(), largeMapOptions());
    appendRecords(env, kRecordCount);
    CHECK(utility::allocatedFileBytes(dataPath) < kAllocationCeiling);
  }

  TEST_CASE("Environment - concurrent first opens of separate environments all succeed",
            "[lmdb][unit][environment][concurrency]")
  {
    constexpr std::size_t kThreadCount = 8;
    auto const temp = ao::test::TempDir{};
    auto failures = std::atomic{std::size_t{0}};
    auto start = std::latch{static_cast<std::ptrdiff_t>(kThreadCount)};
    auto threads = std::vector<std::jthread>{};
    threads.reserve(kThreadCount);

    // One environment per path, as LMDB requires, so this exercises the real open
    // path under contention instead of aliasing one environment across threads.
    for (std::size_t index = 0; index < kThreadCount; ++index)
    {
      auto directory = temp.path() / std::to_string(index);
      REQUIRE(std::filesystem::create_directory(directory));
      threads.emplace_back(
        [directory = std::move(directory), &failures, &start]
        {
          start.arrive_and_wait();
          auto envRes = Environment::open(directory, largeMapOptions());

          if (!envRes || envRes->mapAllocation() != MapAllocation::OnDemand)
          {
            failures.fetch_add(1, std::memory_order_relaxed);
          }
        });
    }

    threads.clear();
    CHECK(failures.load(std::memory_order_relaxed) == 0);
  }

  TEST_CASE("prepareEnvironmentDataFile - a read-only preparation creates nothing", "[lmdb][unit][capacity]")
  {
    auto const temp = ao::test::TempDir{};
    auto const dataPath = temp.path() / "data.mdb";

    // Preparation for a writable environment is allowed to create the file. For a
    // read-only one it is not: a caller asking to read a database that is not
    // there must be told so, not handed an empty one. Only the Windows shim can
    // fail this, because the POSIX shim touches nothing under either access.
    REQUIRE(detail::prepareEnvironmentDataFile(temp.path(), detail::DataFileAccess::ReadOnly));
    CHECK_FALSE(std::filesystem::exists(dataPath));
  }

  TEST_CASE("Environment - opening rejects a flag the data-file preparation is not written for",
            "[lmdb][unit][capacity]")
  {
    auto const temp = ao::test::TempDir{};

    // MDB_NOSUBDIR would make the path the data file rather than its directory,
    // and MDB_WRITEMAP would turn an exhausted volume into a mapping fault. Both
    // break preparation silently, so anything outside the two mirrored flags is
    // refused before an environment exists at all.
    constexpr std::uint32_t kNoSubdir = 0x4000;
    constexpr std::uint32_t kWriteMap = 0x80000;

    for (auto const flag : {kNoSubdir, kWriteMap})
    {
      auto envRes = Environment::open(temp.path(), Environment::Options{.flags = kEnvNoTls | flag, .maxDatabases = 4});

      REQUIRE_FALSE(envRes);
      CHECK(envRes.error().code == Error::Code::InvalidInput);
      CHECK_FALSE(std::filesystem::exists(temp.path() / "data.mdb"));
    }
  }

  TEST_CASE("Environment - a read-only open of an absent database leaves the directory empty", "[lmdb][unit][capacity]")
  {
    auto const temp = ao::test::TempDir{};

    auto envRes =
      Environment::open(temp.path(), Environment::Options{.flags = kEnvNoTls | kEnvReadOnly, .maxDatabases = 4});

    // LMDB owns the diagnostic; what matters here is that reaching it cost the
    // caller no file and no write permission.
    CHECK_FALSE(envRes);
    CHECK_FALSE(std::filesystem::exists(temp.path() / "data.mdb"));
  }
} // namespace ao::lmdb::test
