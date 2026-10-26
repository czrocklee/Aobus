// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "lib/lmdb/detail/EnvironmentDataFile.h"
#include "test/FilesystemTestSupport.h"
#include "test/TestFixtureSupport.h"
#include "test/lmdb/LmdbTestSupport.h"
#include <ao/Error.h>
#include <ao/lmdb/Database.h>
#include <ao/lmdb/Environment.h>
#include <ao/lmdb/Transaction.h>
#include <ao/utility/FileAllocation.h>
#include <ao/utility/Path.h>
#include <ao/utility/ScopedRegistration.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
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
    // Large enough that a data file extended to all of it would be unmistakable,
    // small enough that a build that did so wastes only this much before failing.
    constexpr auto kLargeMapSize = std::size_t{256} * 1024 * 1024;
    constexpr auto kAllocationCeiling = kLargeMapSize / 8;
    constexpr std::size_t kRecordSize = 4096;
    constexpr std::uint32_t kRecordCount = 256;

    // For tests that bound allocation only from above, or not at all: skipping
    // the flush keeps the record appends cheap on Windows and macOS.
    Environment::Options largeMapOptions()
    {
      return Environment::Options{.flags = kEnvNoTls | kEnvNoSync, .maxDatabases = 4, .pinnedMapBytes = kLargeMapSize};
    }

    // For tests that require allocation to cover what was written: Windows can
    // assign space to an unflushed write only later, so each commit is flushed.
    Environment::Options flushedLargeMapOptions()
    {
      return Environment::Options{.flags = kEnvNoTls, .maxDatabases = 4, .pinnedMapBytes = kLargeMapSize};
    }

    // Raises the map to @p floorBytes through the capacity policy after the
    // open, as the library does, rather than pinning it before.
    Environment::Options flushedManagedOptions(std::uint64_t const floorBytes)
    {
      return Environment::Options{.flags = kEnvNoTls, .maxDatabases = 4, .capacity = {.minimumMapBytes = floorBytes}};
    }

    void checkFootprintBelowCeiling(std::filesystem::path const& dataPath)
    {
      CHECK(std::filesystem::file_size(dataPath) < kAllocationCeiling);
      CHECK(utility::allocatedFileBytes(dataPath) < kAllocationCeiling);
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
      auto const expected = createTestData(kRecordSize);
      auto txn = beginWriteTransaction(env);
      auto database = openIntegerKeyDatabase(txn, "records");
      auto writer = database.writer(txn);

      for (std::uint32_t id = 1; id <= count; ++id)
      {
        auto const optValue = writer.get(id);
        REQUIRE(optValue);
        REQUIRE(std::ranges::equal(*optValue, expected));
      }
    }
  } // namespace

  // The data file grows with the pages transactions use, on Windows as well as
  // POSIX, so neither its length nor its allocation follows the map. A Windows
  // build of LMDB with MDB_FIXEDSIZE would extend the file to the map.
  TEST_CASE("Environment - a configured map size does not become disk usage")
  {
    auto const temp = ao::test::TempDir{};
    auto const dataPath = temp.path() / "data.mdb";

    {
      auto env = openEnvironment(temp.path(), flushedLargeMapOptions());
      checkFootprintBelowCeiling(dataPath);
      appendRecords(env, kRecordCount);
    }

    auto const allocated = utility::allocatedFileBytes(dataPath);
    CHECK(allocated >= kRecordSize * kRecordCount);
    CHECK(allocated < kAllocationCeiling);
    CHECK(std::filesystem::file_size(dataPath) < kAllocationCeiling);
  }

  TEST_CASE("Environment - reopening an environment keeps allocation proportional")
  {
    auto const temp = ao::test::TempDir{};
    auto const dataPath = temp.path() / "data.mdb";

    {
      auto env = openEnvironment(temp.path(), flushedLargeMapOptions());
      appendRecords(env, kRecordCount);
    }

    auto const afterFirstSession = utility::allocatedFileBytes(dataPath);

    {
      // Reopening must not extend the existing file to the map either.
      auto env = openEnvironment(temp.path(), flushedLargeMapOptions());
      checkFootprintBelowCeiling(dataPath);
      appendRecords(env, kRecordCount);
      checkFootprintBelowCeiling(dataPath);
    }

    auto const afterSecondSession = utility::allocatedFileBytes(dataPath);
    CHECK(afterSecondSession > afterFirstSession);
    CHECK(afterSecondSession < kAllocationCeiling);
    CHECK(std::filesystem::file_size(dataPath) < kAllocationCeiling);
  }

  TEST_CASE("Environment - a map the capacity policy raises does not become disk usage")
  {
    auto const temp = ao::test::TempDir{};
    auto const dataPath = temp.path() / "data.mdb";

    for (std::uint32_t session = 0; session < 2; ++session)
    {
      INFO("session " << session);
      // Each floor is above the map the previous session recorded, so every open
      // remaps: the first over a fresh file, the second over one that has data.
      auto const floorBytes = std::uint64_t{kLargeMapSize} << session;
      auto env = openEnvironment(temp.path(), flushedManagedOptions(floorBytes));
      REQUIRE(env.capacity().mapBytes == floorBytes);
      checkFootprintBelowCeiling(dataPath);

      if (session > 0)
      {
        requireRecordsReadable(env, kRecordCount);
      }

      appendRecords(env, kRecordCount);
      checkFootprintBelowCeiling(dataPath);
    }

    CHECK(utility::allocatedFileBytes(dataPath) >= kRecordSize * kRecordCount * 2);
    checkFootprintBelowCeiling(dataPath);
  }

  // Windows LMDB commits each page it takes from the end of the map before it
  // writes it, and that extends the file even for a transaction that then
  // aborts. The adapter marks the file sparse so the extension stays a hole,
  // which a volume without sparse files cannot provide. POSIX extends the file
  // only by writing to it.
  TEST_CASE("Environment - an aborted transaction does not become disk usage")
  {
    auto const temp = ao::test::TempDir{};

    if (!ao::test::supportsSparseFiles(temp.path()))
    {
      SKIP("the volume holding the test directory cannot hold a hole");
    }

    auto const dataPath = temp.path() / "data.mdb";
    // One value, which the transaction stages in memory without spilling, so
    // every page it allocated is still unwritten when it aborts.
    constexpr auto kStagedSize = std::size_t{8} * 1024 * 1024;
    constexpr auto kAbortCeiling = kStagedSize / 8;

    {
      auto env = openEnvironment(temp.path(), flushedLargeMapOptions());
      auto txn = beginWriteTransaction(env);
      auto database = openIntegerKeyDatabase(txn, "records");
      REQUIRE(database.writer(txn).append(createTestData(kStagedSize)));
      txn.abort();
      CHECK(utility::allocatedFileBytes(dataPath) < kAbortCeiling);
    }

    // The file keeps the length the abort left it on Windows, so a reopen has
    // to keep that extension a hole too.
    {
      auto env = openEnvironment(temp.path(), flushedLargeMapOptions());
      CHECK(utility::allocatedFileBytes(dataPath) < kAbortCeiling);
    }

    CHECK(utility::allocatedFileBytes(dataPath) < kAbortCeiling);
  }

  TEST_CASE("Environment - opens a path whose name is not ASCII")
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

  TEST_CASE("EnvironmentDataFile - concurrent preparation of one file all succeeds", "[concurrency]")
  {
    constexpr std::size_t kThreadCount = 8;
    auto const temp = ao::test::TempDir{};
    auto failures = std::atomic<std::size_t>{0};
    auto ready = std::latch{static_cast<std::ptrdiff_t>(kThreadCount)};
    auto start = std::latch{1};
    auto threads = std::vector<std::jthread>{};
    threads.reserve(kThreadCount);
    // Release already-created workers before the thread container joins them if
    // construction of a later worker throws.
    auto releaseWorkers = utility::ScopedRegistration{[&start] { start.count_down(); }};

    for (std::size_t index = 0; index < kThreadCount; ++index)
    {
      threads.emplace_back(
        [&temp, &failures, &ready, &start]
        {
          // Every worker is present before release, so they contend for the same
          // create-and-mark sequence rather than arriving in turn.
          ready.count_down();
          start.wait();

          if (!detail::prepareEnvironmentDataFile(temp.path()))
          {
            failures.fetch_add(1, std::memory_order_relaxed);
          }
        });
    }

    ready.wait();
    releaseWorkers.reset();
    threads.clear();

    CHECK(failures.load(std::memory_order_relaxed) == 0);

    // The environment still opens normally over the file the racing callers left
    // behind, and that file still grows only with what is written to it.
    auto env = openEnvironment(temp.path(), largeMapOptions());
    appendRecords(env, kRecordCount);
    checkFootprintBelowCeiling(temp.path() / "data.mdb");
  }

  TEST_CASE("Environment - concurrent first opens of separate environments all succeed", "[concurrency]")
  {
    constexpr std::size_t kThreadCount = 8;
    auto const temp = ao::test::TempDir{};
    auto directories = std::vector<std::filesystem::path>{};
    directories.reserve(kThreadCount);

    for (std::size_t index = 0; index < kThreadCount; ++index)
    {
      auto directory = temp.path() / std::to_string(index);
      REQUIRE(std::filesystem::create_directory(directory));
      directories.push_back(std::move(directory));
    }

    auto failures = std::atomic<std::size_t>{0};
    auto ready = std::latch{static_cast<std::ptrdiff_t>(kThreadCount)};
    auto start = std::latch{1};
    auto threads = std::vector<std::jthread>{};
    threads.reserve(kThreadCount);
    // Release already-created workers before the thread container joins them if
    // construction of a later worker throws.
    auto releaseWorkers = utility::ScopedRegistration{[&start] { start.count_down(); }};

    // One environment per path, as LMDB requires, so this exercises the real open
    // path under contention instead of aliasing one environment across threads.
    for (auto const& directory : directories)
    {
      threads.emplace_back(
        [directory, &failures, &ready, &start]
        {
          ready.count_down();
          start.wait();

          if (!Environment::open(directory, largeMapOptions()))
          {
            failures.fetch_add(1, std::memory_order_relaxed);
          }
        });
    }

    ready.wait();
    releaseWorkers.reset();
    threads.clear();
    CHECK(failures.load(std::memory_order_relaxed) == 0);
  }

  TEST_CASE("Environment - opening rejects a flag outside the mirrored ones")
  {
    auto const temp = ao::test::TempDir{};

    // MDB_NOSUBDIR would make the path the data file rather than its directory,
    // and MDB_WRITEMAP would set the data file's length to the map on POSIX and
    // let an exhausted volume surface as a mapping fault. Both break the
    // adapter's contract silently, so anything outside the mirrored flags is
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

  TEST_CASE("Environment - a read-only open of an absent database leaves the data file absent")
  {
    auto const temp = ao::test::TempDir{};

    auto envRes =
      Environment::open(temp.path(), Environment::Options{.flags = kEnvNoTls | kEnvReadOnly, .maxDatabases = 4});

    // LMDB owns the diagnostic; what matters here is that the failed read-only
    // open did not create its data file.
    CHECK_FALSE(envRes);
    CHECK_FALSE(std::filesystem::exists(temp.path() / "data.mdb"));
  }
} // namespace ao::lmdb::test
