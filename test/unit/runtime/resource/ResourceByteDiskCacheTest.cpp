// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "runtime/resource/ResourceByteDiskCache.h"

#include "test/unit/FilesystemTestSupport.h"
#include "test/unit/TestFixtureSupport.h"
#include <ao/utility/ByteView.h>
#include <ao/utility/Sha256.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <ios>
#include <span>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    constexpr auto kEntryCeiling = std::size_t{1024} * 1024;

    ResourceByteDiskCache makeCache(std::filesystem::path const& root,
                                    std::size_t const byteBudget = std::size_t{64} * 1024)
    {
      return ResourceByteDiskCache{ResourceByteDiskCache::Config{
        .directory = coverCacheDirectory(root),
        .byteBudget = byteBudget,
        .maximumEntryBytes = kEntryCeiling,
      }};
    }

    std::vector<std::byte> filled(std::size_t const byteLength, std::byte const value)
    {
      return std::vector<std::byte>(byteLength, value);
    }

    void writeRaw(std::filesystem::path const& path, std::span<std::byte const> bytes)
    {
      std::filesystem::create_directories(path.parent_path());
      auto stream = std::ofstream{path, std::ios::binary};
      auto const byteView = utility::bytes::stringView(bytes);
      stream.write(byteView.data(), static_cast<std::streamsize>(byteView.size()));
    }

    std::size_t entryCount(std::filesystem::path const& root)
    {
      auto errorCode = std::error_code{};
      std::size_t count = 0;

      for (auto const& entry : std::filesystem::recursive_directory_iterator{coverCacheDirectory(root), errorCode})
      {
        if (entry.is_regular_file(errorCode))
        {
          ++count;
        }
      }

      return count;
    }
  } // namespace

  TEST_CASE("ResourceByteDiskCache - stores and serves content by its digest", "[runtime][unit][resource-cache]")
  {
    auto const temp = ao::test::TempDir{};
    auto const cache = makeCache(temp.path());
    auto const bytes = filled(2048, std::byte{0x5A});
    auto const digest = utility::computeSha256(bytes);

    CHECK(cache.isEnabled());
    CHECK_FALSE(cache.read(digest));

    cache.store(digest, bytes);
    auto const optRead = cache.read(digest);
    REQUIRE(optRead);
    CHECK(*optRead == bytes);

    SECTION("an entry is named by its digest, sharded by the leading byte")
    {
      auto const hex = utility::sha256Hex(digest);
      auto const path = cache.entryPath(digest);

      CHECK(path.filename().string() == hex);
      CHECK(path.parent_path().filename().string() == hex.substr(0, 2));
      CHECK(std::filesystem::exists(path));
    }
  }

  TEST_CASE("ResourceByteDiskCache - an entry whose content does not match its key is discarded, not served",
            "[runtime][unit][resource-cache]")
  {
    auto const temp = ao::test::TempDir{};
    auto const cache = makeCache(temp.path());
    auto const bytes = filled(64, std::byte{0x11});
    auto const digest = utility::computeSha256(bytes);

    // A planted substitute under an existing identity is exactly what the digest
    // is there to refuse, whether it arrived by corruption or by construction.
    writeRaw(cache.entryPath(digest), filled(64, std::byte{0x22}));

    CHECK_FALSE(cache.read(digest));
    CHECK_FALSE(std::filesystem::exists(cache.entryPath(digest)));

    SECTION("an empty entry is the same refusal, which is what a killed writer leaves")
    {
      writeRaw(cache.entryPath(digest), {});
      REQUIRE(std::filesystem::exists(cache.entryPath(digest)));

      CHECK_FALSE(cache.read(digest));
      CHECK_FALSE(std::filesystem::exists(cache.entryPath(digest)));
    }
  }

  TEST_CASE("ResourceByteDiskCache - concurrent stores, reads, and evictions never serve the wrong content",
            "[runtime][unit][resource-cache][concurrency]")
  {
    auto const temp = ao::test::TempDir{};
    constexpr std::size_t kEntryBytes = 4096;
    constexpr std::size_t kThreadCount = 4;
    constexpr std::size_t kPerThread = 16;
    constexpr std::size_t kBudgetEntries = 8;

    // A budget far below what the threads write, so eviction runs against live
    // writers and readers rather than after them.
    auto const cache = makeCache(temp.path(), kBudgetEntries * kEntryBytes);
    auto wrongContent = std::atomic{std::size_t{0}};
    auto threads = std::vector<std::thread>{};
    threads.reserve(kThreadCount);

    for (std::size_t threadIndex = 0; threadIndex < kThreadCount; ++threadIndex)
    {
      threads.emplace_back(
        [&cache, &wrongContent, threadIndex]
        {
          for (std::size_t step = 0; step < kPerThread; ++step)
          {
            auto const bytes = filled(kEntryBytes, static_cast<std::byte>((threadIndex * kPerThread) + step));
            auto const digest = utility::computeSha256(bytes);
            cache.store(digest, bytes);

            // A miss is allowed: another thread's convergence may have evicted
            // this entry already. Answering with anything but these bytes is not.
            if (auto const optRead = cache.read(digest); optRead && *optRead != bytes)
            {
              wrongContent.fetch_add(1);
            }
          }
        });
    }

    for (auto& thread : threads)
    {
      thread.join();
    }

    CHECK(wrongContent.load() == 0);

    // Convergence is approached, not held: a pass can miss entries written after
    // its census, so the settled count is the budget plus at most one entry per
    // writer that was still in flight.
    CHECK(entryCount(temp.path()) <= kBudgetEntries + kThreadCount);
  }

  TEST_CASE("ResourceByteDiskCache - two libraries holding one cover share one entry",
            "[runtime][unit][resource-cache]")
  {
    auto const temp = ao::test::TempDir{};
    auto const firstCache = makeCache(temp.path());
    auto const secondCache = makeCache(temp.path());
    auto const bytes = filled(512, std::byte{0x7E});
    auto const digest = utility::computeSha256(bytes);

    firstCache.store(digest, bytes);
    secondCache.store(digest, bytes);

    CHECK(entryCount(temp.path()) == 1);
    auto const optRead = secondCache.read(digest);
    REQUIRE(optRead);
    CHECK(*optRead == bytes);
  }

  TEST_CASE("ResourceByteDiskCache - the byte budget is converged toward after a write",
            "[runtime][unit][resource-cache]")
  {
    auto const temp = ao::test::TempDir{};
    constexpr std::size_t kEntryBytes = 4096;
    auto const cache = makeCache(temp.path(), 3 * kEntryBytes);

    for (std::size_t index = 0; index < 8; ++index)
    {
      auto const bytes = filled(kEntryBytes, static_cast<std::byte>(index));
      cache.store(utility::computeSha256(bytes), bytes);
    }

    // Brief overshoot is the contract, so the check is that the cache converged
    // rather than that it held the budget at every instant. One entry here is
    // larger than the share of the budget that defers a walk, so every write
    // converges before it returns and the count that outlives the last write is
    // exact rather than a bound.
    CHECK(entryCount(temp.path()) == 3);
  }

  TEST_CASE("ResourceByteDiskCache - convergence is amortized over writes rather than run on each one",
            "[runtime][unit][resource-cache]")
  {
    auto const temp = ao::test::TempDir{};
    constexpr std::size_t kEntryBytes = 1024;
    constexpr auto kBudget = kEntryBytes * ResourceByteDiskCache::kConvergeWriteShare * 4;
    auto const cache = makeCache(temp.path(), kBudget);
    auto const share = kBudget / ResourceByteDiskCache::kConvergeWriteShare;

    auto storeEntry = [&cache](std::size_t const index)
    {
      auto const bytes = filled(kEntryBytes, static_cast<std::byte>(index));
      cache.store(utility::computeSha256(bytes), bytes);
    };

    // The first write of a process converges, so a directory an earlier run left
    // over budget is reclaimed rather than waiting on this one to write enough.
    storeEntry(0);

    // An entry an earlier run could have left behind, well past the budget and
    // stamped cold so the pass below has one unambiguous eviction to make.
    auto const staleBytes = filled(kBudget, std::byte{0xEE});
    auto const stalePath = coverCacheDirectory(temp.path()) / "ff" / "stale";
    writeRaw(stalePath, staleBytes);
    std::filesystem::last_write_time(stalePath, std::filesystem::file_time_type::clock::now() - std::chrono::hours{48});

    std::size_t stored = 1;

    for (; stored * kEntryBytes < share; ++stored)
    {
      storeEntry(stored);
    }

    // Walking the directory after every write costs more than the bytes it
    // reclaims, so writes below the share leave the budget overshot on purpose.
    CHECK(std::filesystem::exists(stalePath));

    storeEntry(stored);

    // Crossing the share is what buys the walk, and the walk is the ordinary
    // least-recently-used pass: the stale entry is both the coldest and far
    // larger than the budget it broke.
    CHECK_FALSE(std::filesystem::exists(stalePath));
  }

  TEST_CASE("ResourceByteDiskCache - eviction is least-recently-used by modification time",
            "[runtime][unit][resource-cache]")
  {
    auto const temp = ao::test::TempDir{};
    constexpr std::size_t kEntryBytes = 4096;
    auto const cache = makeCache(temp.path(), 2 * kEntryBytes);

    auto const oldest = filled(kEntryBytes, std::byte{0x01});
    auto const middle = filled(kEntryBytes, std::byte{0x02});
    auto const newest = filled(kEntryBytes, std::byte{0x03});
    auto const oldestDigest = utility::computeSha256(oldest);
    auto const middleDigest = utility::computeSha256(middle);
    auto const newestDigest = utility::computeSha256(newest);

    cache.store(oldestDigest, oldest);
    cache.store(middleDigest, middle);

    // Modification time is the recency signal, so an explicit stamp is how a test
    // states which entry is coldest without sleeping.
    auto const now = std::filesystem::file_time_type::clock::now();
    std::filesystem::last_write_time(cache.entryPath(oldestDigest), now - std::chrono::hours{48});
    std::filesystem::last_write_time(cache.entryPath(middleDigest), now - std::chrono::hours{1});

    cache.store(newestDigest, newest);

    CHECK_FALSE(std::filesystem::exists(cache.entryPath(oldestDigest)));
    CHECK(std::filesystem::exists(cache.entryPath(middleDigest)));
    CHECK(std::filesystem::exists(cache.entryPath(newestDigest)));
  }

  TEST_CASE("ResourceByteDiskCache - a hit rewrites the modification time at most once a day",
            "[runtime][unit][resource-cache]")
  {
    auto const temp = ao::test::TempDir{};
    auto const cache = makeCache(temp.path());
    auto const bytes = filled(128, std::byte{0x33});
    auto const digest = utility::computeSha256(bytes);
    cache.store(digest, bytes);
    auto const path = cache.entryPath(digest);

    SECTION("a fresh entry is left alone, so a warm cover is not rewritten on every display")
    {
      auto const beforeTime = std::filesystem::last_write_time(path);
      REQUIRE(cache.read(digest));
      CHECK(std::filesystem::last_write_time(path) == beforeTime);
    }

    SECTION("an entry older than the interval is touched")
    {
      auto const staleTime =
        std::filesystem::file_time_type::clock::now() - (ResourceByteDiskCache::kTouchInterval * 2);
      std::filesystem::last_write_time(path, staleTime);
      REQUIRE(cache.read(digest));
      CHECK(std::filesystem::last_write_time(path) > staleTime);
    }
  }

  TEST_CASE("ResourceByteDiskCache - an entry above the maximum is never written", "[runtime][unit][resource-cache]")
  {
    auto const temp = ao::test::TempDir{};
    auto const cache = ResourceByteDiskCache{ResourceByteDiskCache::Config{
      .directory = coverCacheDirectory(temp.path()),
      .maximumEntryBytes = 1024,
    }};
    auto const bytes = filled(1025, std::byte{0x44});
    auto const digest = utility::computeSha256(bytes);

    cache.store(digest, bytes);

    // One cover no frontend can display must not take a share of the budget.
    CHECK_FALSE(cache.read(digest));
    CHECK(entryCount(temp.path()) == 0);
  }

  TEST_CASE("ResourceByteDiskCache - an entry above the maximum is still served once it is there",
            "[runtime][unit][resource-cache]")
  {
    auto const temp = ao::test::TempDir{};
    auto const cache = ResourceByteDiskCache{ResourceByteDiskCache::Config{
      .directory = coverCacheDirectory(temp.path()),
      .maximumEntryBytes = 1024,
    }};
    auto const bytes = filled(1025, std::byte{0x88});
    auto const digest = utility::computeSha256(bytes);

    // The maximum governs what this cache installs, not what it may serve. A read
    // that applied it would put a ceiling on raw export, which the specification
    // exempts and which is served from this same tier; deciding an
    // entry's fate by its length would also make that decision here rather than
    // in the caller that knows its own limit.
    writeRaw(cache.entryPath(digest), bytes);

    auto const optRead = cache.read(digest);
    REQUIRE(optRead);
    CHECK(*optRead == bytes);
  }

  TEST_CASE("ResourceByteDiskCache - a shard it cannot enumerate defers convergence rather than failing the write",
            "[runtime][unit][resource-cache]")
  {
    auto const temp = ao::test::TempDir{};
    constexpr std::size_t kEntryBytes = 4096;
    auto const cache = makeCache(temp.path(), 2 * kEntryBytes);

    auto const blocked = filled(kEntryBytes, std::byte{0x01});
    auto const blockedDigest = utility::computeSha256(blocked);
    auto const stored = filled(kEntryBytes, std::byte{0x02});
    auto const storedDigest = utility::computeSha256(stored);
    REQUIRE(cache.entryPath(blockedDigest).parent_path() != cache.entryPath(storedDigest).parent_path());

    cache.store(blockedDigest, blocked);

    auto const denied = ao::test::ScopedDirectoryAccessGuard{
      cache.entryPath(blockedDigest).parent_path(), ao::test::DeniedDirectoryAccess::Read};

    if (!denied.effective())
    {
      SKIP("the current process bypasses directory permissions");
    }

    // The census now walks into a shard it cannot open, which is what a lost
    // permission or a removable mount looks like from inside `store`. The caller
    // already holds verified bytes, so the pass ends quietly and the entry stays.
    REQUIRE_NOTHROW(cache.store(storedDigest, stored));
    auto const optStored = cache.read(storedDigest);
    REQUIRE(optStored);
    CHECK(*optStored == stored);
  }

  TEST_CASE("ResourceByteDiskCache - a cache with no directory is inert rather than broken",
            "[runtime][unit][resource-cache]")
  {
    auto const cache = ResourceByteDiskCache{ResourceByteDiskCache::Config{}};
    auto const bytes = filled(16, std::byte{0x55});
    auto const digest = utility::computeSha256(bytes);

    CHECK_FALSE(cache.isEnabled());
    cache.store(digest, bytes);
    CHECK_FALSE(cache.read(digest));
  }

  TEST_CASE("ResourceByteDiskCache - an unwritable directory installs nothing and reports no failure",
            "[runtime][unit][resource-cache]")
  {
    auto const temp = ao::test::TempDir{};
    auto const cache = makeCache(temp.path());
    auto const bytes = filled(32, std::byte{0x66});
    auto const digest = utility::computeSha256(bytes);

    // A file where the shard directory belongs makes creation fail the way a
    // read-only mount or a lost permission would.
    auto const shardPath = cache.entryPath(digest).parent_path();
    std::filesystem::create_directories(shardPath.parent_path());
    writeRaw(shardPath, bytes);

    cache.store(digest, bytes);

    // The caller already holds verified bytes; a cache that cannot retain them
    // must not turn that into a failure, so the only consequence is a cold miss.
    CHECK_FALSE(cache.read(digest));
  }

  TEST_CASE("ResourceByteDiskCache - deleting the directory loses only the cache", "[runtime][unit][resource-cache]")
  {
    auto const temp = ao::test::TempDir{};
    auto const cache = makeCache(temp.path());
    auto const bytes = filled(256, std::byte{0x77});
    auto const digest = utility::computeSha256(bytes);
    cache.store(digest, bytes);
    auto const optBeforeDelete = cache.read(digest);
    REQUIRE(optBeforeDelete);
    CHECK(*optBeforeDelete == bytes);

    std::filesystem::remove_all(coverCacheDirectory(temp.path()));

    CHECK_FALSE(cache.read(digest));
    cache.store(digest, bytes);
    auto const optAfterRecreate = cache.read(digest);
    REQUIRE(optAfterRecreate);
    CHECK(*optAfterRecreate == bytes);
  }

  TEST_CASE("coverCacheDirectory - an empty root stays empty", "[runtime][unit][resource-cache]")
  {
    CHECK(coverCacheDirectory({}).empty());
    CHECK(coverCacheDirectory("/tmp/aobus") == std::filesystem::path{"/tmp/aobus"} / "cover");
  }
} // namespace ao::rt::test
