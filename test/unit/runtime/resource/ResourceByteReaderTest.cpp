// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "runtime/resource/ResourceByteReader.h"

#include "runtime/resource/ResourceByteDiskCache.h"
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
#include <ao/utility/Sha256.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
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
    constexpr auto kLargeResourceCompletionTimeout = std::chrono::seconds{10};

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
      auto const cache = ResourceByteDiskCache{ResourceByteDiskCache::Config{
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

    struct ReadObservation final
    {
      Result<std::optional<std::vector<std::byte>>> result;
      bool completedOnExecutor = false;
    };

    async::Task<ReadObservation> readResourceAndObserveExecutor(ResourceByteReader* reader,
                                                                async::Executor* executor,
                                                                ResourceId const resourceId)
    {
      auto result = co_await reader->readInteractiveAsync(resourceId);
      co_return ReadObservation{.result = std::move(result), .completedOnExecutor = executor->isCurrent()};
    }

    template<typename T>
    async::Task<T> countCompletion(std::shared_ptr<std::atomic<std::size_t>> counterPtr, async::Task<T> task)
    {
      auto valueRes = co_await std::move(task);
      counterPtr->fetch_add(1);
      co_return valueRes;
    }
  } // namespace

  TEST_CASE("ResourceByteReader - interactive reads return owned bytes on the callback executor",
            "[runtime][unit][resource][concurrency]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const bytes = std::array{std::byte{0x10}, std::byte{0x20}, std::byte{0x30}};
    auto const resourceId = writeResource(libraryFixture.library(), bytes);
    auto const cacheRoot = libraryFixture.root() / "cache";
    installCacheEntry(cacheRoot, bytes);
    auto executor = QueuedExecutor{};
    auto runtime = async::Runtime{executor};
    auto reader = ResourceByteReader{runtime, libraryFixture.library(), cacheRoot};
    auto completedPtr = std::make_shared<std::atomic_bool>(false);
    auto future = spawnFuture(runtime, readResourceAndObserveExecutor(&reader, &executor, resourceId), completedPtr);

    REQUIRE(executor.drainUntil([&completedPtr] { return isReady(completedPtr); }));
    auto observation = future.get();
    REQUIRE(observation.result);
    REQUIRE(*observation.result);
    CHECK(observation.completedOnExecutor);

    auto missingCompletedPtr = std::make_shared<std::atomic_bool>(false);
    auto missingFuture = spawnFuture(runtime, reader.readInteractiveAsync(ResourceId{987654}), missingCompletedPtr);
    REQUIRE(executor.drainUntil([&missingCompletedPtr] { return isReady(missingCompletedPtr); }));
    auto missingRes = missingFuture.get();
    REQUIRE(missingRes);
    CHECK_FALSE(*missingRes);

    auto invalidCompletedPtr = std::make_shared<std::atomic_bool>(false);
    auto invalidFuture = spawnFuture(runtime, reader.readInteractiveAsync(kInvalidResourceId), invalidCompletedPtr);
    REQUIRE(executor.drainUntil([&invalidCompletedPtr] { return isReady(invalidCompletedPtr); }));
    auto invalidRes = invalidFuture.get();
    REQUIRE(invalidRes);
    CHECK_FALSE(*invalidRes);

    runtime.requestStop();
    runtime.join();
  }

  TEST_CASE("ResourceByteReader - interactive encoded-byte limit is exact", "[runtime][unit][resource]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto executor = QueuedExecutor{};
    auto runtime = async::Runtime{executor};
    auto const cacheRoot = libraryFixture.root() / "cache";

    SECTION("bytes at the limit are returned")
    {
      auto bytes = std::vector<std::byte>(kMaximumInteractiveResourceBytes, std::byte{0x4A});
      auto const resourceId = writeResource(libraryFixture.library(), bytes);
      installCacheEntry(cacheRoot, bytes);
      auto reader = ResourceByteReader{runtime, libraryFixture.library(), cacheRoot};
      auto result =
        runQueuedTask(runtime, executor, reader.readInteractiveAsync(resourceId), kLargeResourceCompletionTimeout);

      REQUIRE(result);
      REQUIRE(*result);
      CHECK((*result)->size() == kMaximumInteractiveResourceBytes);
      CHECK((*result)->front() == std::byte{0x4A});
      CHECK((*result)->back() == std::byte{0x4A});
    }

    SECTION("bytes above the limit are rejected before publication")
    {
      auto bytes = std::vector<std::byte>(kMaximumInteractiveResourceBytes + 1, std::byte{0x5B});
      auto const resourceId = writeResource(libraryFixture.library(), bytes);

      // The interactive cache refuses an entry no frontend may serve. Install it
      // through an administrative-sized cache so both public limits exercise the
      // same verified read.
      auto const oversizedCache = ResourceByteDiskCache{ResourceByteDiskCache::Config{
        .directory = coverCacheDirectory(cacheRoot),
        .maximumEntryBytes = bytes.size(),
      }};
      oversizedCache.store(utility::computeSha256(bytes), bytes);
      auto reader = ResourceByteReader{runtime, libraryFixture.library(), cacheRoot};
      auto result =
        runQueuedTask(runtime, executor, reader.readInteractiveAsync(resourceId), kLargeResourceCompletionTimeout);

      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::ValueTooLarge);

      auto exportRes =
        runQueuedTask(runtime, executor, reader.readForExportAsync(resourceId), kLargeResourceCompletionTimeout);
      REQUIRE(exportRes);
      REQUIRE(*exportRes);
      CHECK((*exportRes)->size() == bytes.size());
    }

    SECTION("a descriptor with no cache entry and no carrier yields no image")
    {
      auto const bytes = std::array{std::byte{0x01}, std::byte{0x02}, std::byte{0x03}};
      auto const resourceId = writeResource(libraryFixture.library(), bytes);
      auto reader = ResourceByteReader{runtime, libraryFixture.library(), cacheRoot};
      auto result = runQueuedTask(runtime, executor, reader.readInteractiveAsync(resourceId));

      REQUIRE(result);
      CHECK_FALSE(*result);
    }

    runtime.requestStop();
    runtime.join();
  }

  TEST_CASE("ResourceByteReader - the carrier index is built lazily and rebuilt once per revision",
            "[runtime][unit][resource][resource-walk]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const bytes = std::array{std::byte{0xA1}, std::byte{0xB2}};
    auto const resourceId = writeResource(libraryFixture.library(), bytes);
    auto executor = QueuedExecutor{};
    auto runtime = async::Runtime{executor};
    auto reader = ResourceByteReader{runtime, libraryFixture.library(), libraryFixture.root() / "cache"};

    CHECK(reader.carrierIndexBuildCount() == 0);
    REQUIRE(runQueuedTask(runtime, executor, reader.readInteractiveAsync(resourceId)));
    CHECK(reader.carrierIndexBuildCount() == 1);

    SECTION("a second request at the same revision reuses the snapshot")
    {
      REQUIRE(runQueuedTask(runtime, executor, reader.readInteractiveAsync(resourceId)));
      CHECK(reader.carrierIndexBuildCount() == 1);
    }

    SECTION("a request after the revision moves sees a new snapshot")
    {
      auto const revisionBytes = std::array{std::byte{0xEE}};
      std::ignore = writeResource(libraryFixture.library(), revisionBytes);
      REQUIRE(runQueuedTask(runtime, executor, reader.readInteractiveAsync(resourceId)));
      CHECK(reader.carrierIndexBuildCount() == 2);
    }

    runtime.requestStop();
    runtime.join();
  }

  TEST_CASE("ResourceByteReader - one stale stamp costs one index build across several workers",
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
    auto reader = ResourceByteReader{runtime, libraryFixture.library(), libraryFixture.root() / "cache"};
    auto completedCountPtr = std::make_shared<std::atomic<std::size_t>>(0);
    auto futures = std::vector<async::TaskFuture<Result<std::optional<std::vector<std::byte>>>>>{};
    futures.reserve(kRequestCount);

    for (std::size_t request = 0; request < kRequestCount; ++request)
    {
      futures.push_back(runtime.spawn(countCompletion(completedCountPtr, reader.readInteractiveAsync(resourceId))));
    }

    REQUIRE(executor.drainUntil(
      [&completedCountPtr] { return completedCountPtr->load() == kRequestCount; }, kLargeResourceCompletionTimeout));

    for (auto& future : futures)
    {
      REQUIRE(future.get());
    }

    CHECK(reader.carrierIndexBuildCount() == 1);

    runtime.requestStop();
    runtime.join();
  }

  TEST_CASE("ResourceByteReader - cancelling an interactive read suppresses completion",
            "[runtime][regression][resource][concurrency]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const bytes = std::array{std::byte{0x01}, std::byte{0x02}};
    auto const resourceId = writeResource(libraryFixture.library(), bytes);
    auto executor = QueuedExecutor{};
    auto runtime = async::Runtime{executor};
    auto reader = ResourceByteReader{runtime, libraryFixture.library(), {}};
    auto stopSource = std::stop_source{};
    auto completedPtr = std::make_shared<std::atomic_bool>(false);
    auto future = spawnFuture(runtime, reader.readInteractiveAsync(resourceId, stopSource.get_token()), completedPtr);
    executor.checkQueued();

    REQUIRE(stopSource.request_stop());
    REQUIRE(executor.drainUntil([&completedPtr] { return isReady(completedPtr); }));
    CHECK_THROWS_AS(std::ignore = future.get(), async::OperationCancelled);

    runtime.requestStop();
    runtime.join();
  }
} // namespace ao::rt::test
