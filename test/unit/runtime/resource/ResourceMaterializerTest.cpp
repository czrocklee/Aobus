// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "runtime/resource/ResourceMaterializer.h"

#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/library/WritableLibraryTestSupport.h"
#include "test/unit/runtime/AsyncTestSupport.h"
#include "test/unit/runtime/ExecutorTestSupport.h"
#include "test/unit/runtime/RuntimeLibraryTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Executor.h>
#include <ao/async/OperationCancelled.h>
#include <ao/async/Runtime.h>
#include <ao/async/Task.h>
#include <ao/async/TaskFuture.h>
#include <ao/library/ResourceStore.h>
#include <ao/rt/resource/ResourceBytes.h>
#include <ao/rt/resource/ResourceDiskCache.h>
#include <ao/utility/Sha256.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <utility>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    ResourceId writeResource(library::MusicLibrary& library, std::span<std::byte const> bytes)
    {
      auto transaction = library::test::writeTransaction(library);
      auto result = library::test::physicalWriter(library.resources(), transaction).create(bytes);
      REQUIRE(result);
      REQUIRE(transaction.commit());
      return *result;
    }

    /** Installs bytes through the same derived-cache tier a real session fills. */
    void installCacheEntry(std::filesystem::path const& cacheRoot, std::span<std::byte const> bytes)
    {
      auto const cache = ResourceDiskCache{ResourceDiskCache::Config{
        .directory = coverCacheDirectory(cacheRoot),
        .maximumEntryBytes = kMaximumInteractiveResourceBytes,
      }};
      cache.store(utility::computeSha256(bytes), bytes);
    }

    template<typename T>
    auto spawnFuture(async::Runtime& runtime,
                     async::Task<T> task,
                     std::shared_ptr<std::atomic_bool> const& completedPtr)
    {
      return runtime.spawn(flagCompletion(completedPtr, std::move(task)));
    }

    bool isReady(std::shared_ptr<std::atomic_bool> const& completedPtr)
    {
      return completedPtr->load();
    }

    async::Task<bool> loadResourceAndCheckExecutor(ResourceMaterializer* materializer,
                                                   async::Executor* executor,
                                                   ResourceId const resourceId)
    {
      auto result = co_await materializer->loadInteractiveAsync(resourceId);
      REQUIRE(result);
      REQUIRE(*result);
      co_return executor->isCurrent();
    }

    template<typename T>
    async::Task<T> countCompletion(std::shared_ptr<std::atomic<std::size_t>> counterPtr, async::Task<T> task)
    {
      auto valueRes = co_await std::move(task);
      counterPtr->fetch_add(1);
      co_return valueRes;
    }
  } // namespace

  TEST_CASE("ResourceMaterializer - interactive reads return owned bytes on the callback executor",
            "[runtime][unit][resource][concurrency]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const bytes = std::array{std::byte{0x10}, std::byte{0x20}, std::byte{0x30}};
    auto const resourceId = writeResource(libraryFixture.library(), bytes);
    auto const cacheRoot = libraryFixture.root() / "cache";
    installCacheEntry(cacheRoot, bytes);
    auto executor = QueuedExecutor{};
    auto runtime = async::Runtime{executor};
    auto materializer = ResourceMaterializer{runtime, libraryFixture.library(), cacheRoot};
    auto completedPtr = std::make_shared<std::atomic_bool>(false);
    auto future =
      spawnFuture(runtime, loadResourceAndCheckExecutor(&materializer, &executor, resourceId), completedPtr);

    REQUIRE(executor.drainUntil([&completedPtr] { return isReady(completedPtr); }));
    CHECK(future.get());

    auto missingCompletedPtr = std::make_shared<std::atomic_bool>(false);
    auto missingFuture =
      spawnFuture(runtime, materializer.loadInteractiveAsync(ResourceId{987654}), missingCompletedPtr);
    REQUIRE(executor.drainUntil([&missingCompletedPtr] { return isReady(missingCompletedPtr); }));
    auto missingRes = missingFuture.get();
    REQUIRE(missingRes);
    CHECK_FALSE(*missingRes);

    auto invalidCompletedPtr = std::make_shared<std::atomic_bool>(false);
    auto invalidFuture =
      spawnFuture(runtime, materializer.loadInteractiveAsync(kInvalidResourceId), invalidCompletedPtr);
    REQUIRE(executor.drainUntil([&invalidCompletedPtr] { return isReady(invalidCompletedPtr); }));
    auto invalidRes = invalidFuture.get();
    REQUIRE(invalidRes);
    CHECK_FALSE(*invalidRes);

    runtime.requestStop();
    runtime.join();
  }

  TEST_CASE("ResourceMaterializer - interactive encoded-byte limit is exact", "[runtime][unit][resource]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto executor = QueuedExecutor{};
    auto runtime = async::Runtime{executor};
    auto const cacheRoot = libraryFixture.root() / "cache";

    SECTION("materialized bytes at the limit are returned")
    {
      auto bytes = std::vector<std::byte>(kMaximumInteractiveResourceBytes, std::byte{0x4A});
      auto const resourceId = writeResource(libraryFixture.library(), bytes);
      installCacheEntry(cacheRoot, bytes);
      auto materializer = ResourceMaterializer{runtime, libraryFixture.library(), cacheRoot};
      auto result = runQueuedTask(runtime, executor, materializer.loadInteractiveAsync(resourceId));

      REQUIRE(result);
      REQUIRE(*result);
      CHECK((*result)->size() == kMaximumInteractiveResourceBytes);
      CHECK((*result)->front() == std::byte{0x4A});
      CHECK((*result)->back() == std::byte{0x4A});
    }

    SECTION("materialized bytes above the limit are rejected before publication")
    {
      auto bytes = std::vector<std::byte>(kMaximumInteractiveResourceBytes + 1, std::byte{0x5B});
      auto const resourceId = writeResource(libraryFixture.library(), bytes);

      // The interactive cache refuses an entry no frontend may serve. Install it
      // through an administrative-sized cache so both public limits exercise the
      // same materialization walk.
      auto const oversizedCache = ResourceDiskCache{ResourceDiskCache::Config{
        .directory = coverCacheDirectory(cacheRoot),
        .maximumEntryBytes = bytes.size(),
      }};
      oversizedCache.store(utility::computeSha256(bytes), bytes);
      auto materializer = ResourceMaterializer{runtime, libraryFixture.library(), cacheRoot};
      auto result = runQueuedTask(runtime, executor, materializer.loadInteractiveAsync(resourceId));

      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::ValueTooLarge);

      auto administrativeRes = runQueuedTask(runtime, executor, materializer.loadAdministrativeAsync(resourceId));
      REQUIRE(administrativeRes);
      REQUIRE(*administrativeRes);
      CHECK((*administrativeRes)->size() == bytes.size());
    }

    SECTION("a descriptor with no cache entry and no carrier yields no image")
    {
      auto const bytes = std::array{std::byte{0x01}, std::byte{0x02}, std::byte{0x03}};
      auto const resourceId = writeResource(libraryFixture.library(), bytes);
      auto materializer = ResourceMaterializer{runtime, libraryFixture.library(), cacheRoot};
      auto result = runQueuedTask(runtime, executor, materializer.loadInteractiveAsync(resourceId));

      REQUIRE(result);
      CHECK_FALSE(*result);
    }

    runtime.requestStop();
    runtime.join();
  }

  TEST_CASE("ResourceMaterializer - the carrier index is built lazily and rebuilt once per revision",
            "[runtime][unit][resource][resource-walk]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const bytes = std::array{std::byte{0xA1}, std::byte{0xB2}};
    auto const resourceId = writeResource(libraryFixture.library(), bytes);
    auto executor = QueuedExecutor{};
    auto runtime = async::Runtime{executor};
    auto materializer = ResourceMaterializer{runtime, libraryFixture.library(), libraryFixture.root() / "cache"};

    CHECK(materializer.carrierIndexBuildCount() == 0);
    REQUIRE(runQueuedTask(runtime, executor, materializer.loadInteractiveAsync(resourceId)));
    CHECK(materializer.carrierIndexBuildCount() == 1);

    SECTION("a second request at the same revision reuses the snapshot")
    {
      REQUIRE(runQueuedTask(runtime, executor, materializer.loadInteractiveAsync(resourceId)));
      CHECK(materializer.carrierIndexBuildCount() == 1);
    }

    SECTION("a request after the revision moves sees a new snapshot")
    {
      auto const revisionBytes = std::array{std::byte{0xEE}};
      std::ignore = writeResource(libraryFixture.library(), revisionBytes);
      REQUIRE(runQueuedTask(runtime, executor, materializer.loadInteractiveAsync(resourceId)));
      CHECK(materializer.carrierIndexBuildCount() == 2);
    }

    runtime.requestStop();
    runtime.join();
  }

  TEST_CASE("ResourceMaterializer - one stale stamp costs one index build across several workers",
            "[runtime][unit][resource][concurrency]")
  {
    constexpr std::size_t kRequestCount = 50;
    auto libraryFixture = MusicLibraryFixture{};
    auto const bytes = std::array{std::byte{0xC3}, std::byte{0xD4}};
    auto const resourceId = writeResource(libraryFixture.library(), bytes);
    auto executor = QueuedExecutor{};

    // This observes the multiple-worker row of the concurrency matrix. The
    // internal rebuild mutex remains the mechanism guaranteeing one publication.
    auto runtime = async::Runtime{executor, 4};
    auto materializer = ResourceMaterializer{runtime, libraryFixture.library(), libraryFixture.root() / "cache"};
    auto completedCountPtr = std::make_shared<std::atomic<std::size_t>>(0);
    auto futures = std::vector<async::TaskFuture<Result<std::optional<std::vector<std::byte>>>>>{};
    futures.reserve(kRequestCount);

    for (std::size_t request = 0; request < kRequestCount; ++request)
    {
      futures.push_back(
        runtime.spawn(countCompletion(completedCountPtr, materializer.loadInteractiveAsync(resourceId))));
    }

    REQUIRE(executor.drainUntil([&completedCountPtr] { return completedCountPtr->load() == kRequestCount; }));

    for (auto& future : futures)
    {
      REQUIRE(future.get());
    }

    CHECK(materializer.carrierIndexBuildCount() == 1);

    runtime.requestStop();
    runtime.join();
  }

  TEST_CASE("ResourceMaterializer - cancelling an interactive read suppresses completion",
            "[runtime][regression][resource][concurrency]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const bytes = std::array{std::byte{0x01}, std::byte{0x02}};
    auto const resourceId = writeResource(libraryFixture.library(), bytes);
    auto executor = QueuedExecutor{};
    auto runtime = async::Runtime{executor};
    auto materializer = ResourceMaterializer{runtime, libraryFixture.library(), {}};
    auto stopSource = std::stop_source{};
    auto completedPtr = std::make_shared<std::atomic_bool>(false);
    auto future =
      spawnFuture(runtime, materializer.loadInteractiveAsync(resourceId, stopSource.get_token()), completedPtr);
    executor.checkQueued();

    REQUIRE(stopSource.request_stop());
    REQUIRE(executor.drainUntil([&completedPtr] { return isReady(completedPtr); }));
    CHECK_THROWS_AS(std::ignore = future.get(), async::OperationCancelled);

    runtime.requestStop();
    runtime.join();
  }
} // namespace ao::rt::test
