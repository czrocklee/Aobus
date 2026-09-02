// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/rt/resource/ResourceByteMemoryCache.h>

#include "runtime/resource/ResourceByteDiskCache.h"
#include "runtime/resource/ResourceByteReader.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/library/MusicLibraryTestSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/library/WritableLibraryTestSupport.h"
#include "test/unit/runtime/AsyncTestSupport.h"
#include "test/unit/runtime/ExecutorTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/OperationCancelled.h>
#include <ao/async/Runtime.h>
#include <ao/async/Task.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/ResourceStore.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ConfigStore.h>
#include <ao/rt/CoreRuntime.h>
#include <ao/rt/library/LibraryPaths.h>
#include <ao/rt/resource/ResourceBytes.h>
#include <ao/utility/Sha256.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
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
    class RuntimeOwner final
    {
    public:
      RuntimeOwner()
      {
        auto executorPtr = std::make_unique<QueuedExecutor>();
        _executor = executorPtr.get();
        _runtimePtr = std::make_shared<CoreRuntime>(
          ao::test::requireValue(CoreRuntime::create(std::move(executorPtr),
                                                     _tempDir.path(),
                                                     LibraryPaths{_tempDir.path()}.databasePath(),
                                                     _tempDir.path() / "cache",
                                                     library::test::kTestMusicLibraryMapBytes)));
      }

      ~RuntimeOwner()
      {
        _runtimePtr->async().requestStop();
        _runtimePtr->async().join();
      }

      RuntimeOwner(RuntimeOwner const&) = delete;
      RuntimeOwner& operator=(RuntimeOwner const&) = delete;
      RuntimeOwner(RuntimeOwner&&) = delete;
      RuntimeOwner& operator=(RuntimeOwner&&) = delete;

      QueuedExecutor& executor() const noexcept { return *_executor; }
      std::shared_ptr<CoreRuntime> const& runtimePtr() const noexcept { return _runtimePtr; }

    private:
      ao::test::TempDir _tempDir;
      QueuedExecutor* _executor = nullptr;
      std::shared_ptr<CoreRuntime> _runtimePtr;
    };

    ResourceId writeResource(std::filesystem::path const& musicRoot,
                             std::filesystem::path const& databasePath,
                             std::span<std::byte const> const bytes)
    {
      auto library = library::test::makeTestMusicLibrary(musicRoot, databasePath);
      auto transaction = library::test::writeTransaction(library);
      auto result = library::test::physicalWriter(library.resources(), transaction).create(bytes);
      REQUIRE(result);
      REQUIRE(transaction.commit());
      return *result;
    }

    /// The row names content and holds none, so a read has to find it somewhere.
    /// The derived cache is the tier that needs no media file.
    void installCacheEntry(std::filesystem::path const& cacheRoot, std::span<std::byte const> bytes)
    {
      auto const cache = ResourceByteDiskCache{ResourceByteDiskCache::Config{
        .directory = coverCacheDirectory(cacheRoot),
        .maximumEntryBytes = kMaximumInteractiveResourceBytes,
      }};
      cache.store(utility::computeSha256(bytes), bytes);
    }

    async::Task<Result<std::optional<std::vector<std::byte>>>> waitForRelease(AsyncTestState<std::size_t> readCount,
                                                                              AsyncBarrier* release,
                                                                              std::vector<std::byte> bytes,
                                                                              std::stop_token const stopToken)
    {
      readCount.increment();
      release->wait();
      async::throwIfStopRequested(stopToken);
      co_return std::optional{std::move(bytes)};
    }

    async::Task<Result<std::optional<std::vector<std::byte>>>> readAfterRelease(AsyncTestState<std::size_t> readCount,
                                                                                AsyncBarrier* release,
                                                                                std::vector<std::byte> bytes,
                                                                                ResourceId /*resourceId*/,
                                                                                std::stop_token const stopToken)
    {
      return waitForRelease(std::move(readCount), release, std::move(bytes), stopToken);
    }

    async::Task<Result<std::optional<std::vector<std::byte>>>> failOneRead(
      std::shared_ptr<std::atomic_bool> failNextPtr,
      AsyncTestState<std::size_t> readCount,
      std::vector<std::byte> bytes)
    {
      readCount.increment();

      if (failNextPtr->exchange(false))
      {
        co_return makeError(Error::Code::IoError, "injected resource read failure");
      }

      co_return std::optional{std::move(bytes)};
    }

    async::Task<Result<std::optional<std::vector<std::byte>>>> readAfterOneFailure(
      std::shared_ptr<std::atomic_bool> failNextPtr,
      AsyncTestState<std::size_t> readCount,
      std::vector<std::byte> bytes,
      ResourceId /*resourceId*/,
      std::stop_token /*stopToken*/)
    {
      return failOneRead(std::move(failNextPtr), std::move(readCount), std::move(bytes));
    }

    async::Task<Result<std::optional<std::vector<std::byte>>>> returnBytes(std::vector<std::byte> bytes)
    {
      co_return std::optional{std::move(bytes)};
    }

    async::Task<Result<std::optional<std::vector<std::byte>>>> readCustomBytes(std::vector<std::byte> bytes,
                                                                               ResourceId /*resourceId*/,
                                                                               std::stop_token /*stopToken*/)
    {
      return returnBytes(std::move(bytes));
    }

    async::Task<Result<std::optional<std::vector<std::byte>>>> cancelReadTask(AsyncTestState<std::size_t> readCount)
    {
      readCount.increment();
      async::throwOperationCancelled();
      co_return std::optional<std::vector<std::byte>>{};
    }

    async::Task<Result<std::optional<std::vector<std::byte>>>> cancelRead(AsyncTestState<std::size_t> readCount,
                                                                          ResourceId /*resourceId*/,
                                                                          std::stop_token /*stopToken*/)
    {
      return cancelReadTask(std::move(readCount));
    }

    async::Task<Result<std::optional<std::vector<std::byte>>>> readAcrossRebindTask(
      AsyncTestState<std::size_t> readCount,
      AsyncTestState<bool> firstReadReleased,
      AsyncBarrier* release,
      std::stop_token const stopToken)
    {
      if (auto const attempt = readCount.increment(); attempt == 1)
      {
        release->wait();
        firstReadReleased.set(true);
        async::throwIfStopRequested(stopToken);
      }

      co_return std::optional{std::vector{std::byte{0x21}, std::byte{0x22}}};
    }

    async::Task<Result<std::optional<std::vector<std::byte>>>> readAcrossRebind(AsyncTestState<std::size_t> readCount,
                                                                                AsyncTestState<bool> firstReadReleased,
                                                                                AsyncBarrier* release,
                                                                                ResourceId /*resourceId*/,
                                                                                std::stop_token const stopToken)
    {
      return readAcrossRebindTask(std::move(readCount), std::move(firstReadReleased), release, stopToken);
    }

    async::Task<Result<std::optional<std::vector<std::byte>>>> readSizedBytes(AsyncTestState<std::size_t> readCount,
                                                                              bool const oversizedFourthResource,
                                                                              ResourceId const resourceId,
                                                                              std::stop_token /*stopToken*/)
    {
      readCount.increment();
      auto const byteCount = oversizedFourthResource && resourceId == ResourceId{4} ? std::size_t{6} : std::size_t{2};
      co_return std::optional{std::vector<std::byte>(byteCount, std::byte{0x4A})};
    }

    std::vector<std::byte> requestAndWait(ResourceByteMemoryCache& cache,
                                          RuntimeOwner& owner,
                                          ResourceId const resourceId)
    {
      auto received = std::vector<std::byte>{};
      bool completed = false;
      auto request = cache.request(resourceId,
                                   [&](ResourceBytes bytes)
                                   {
                                     received.assign(bytes.view().begin(), bytes.view().end());
                                     completed = true;
                                   });

      if (!completed)
      {
        REQUIRE(request);
        REQUIRE(owner.executor().drainUntil([&] { return completed; }));
      }

      return received;
    }
  } // namespace

  TEST_CASE("ResourceByteMemoryCache - default runtime read delivers exact bytes on the callback executor",
            "[runtime][unit][resource-byte][concurrency]")
  {
    auto tempDir = ao::test::TempDir{};
    auto const paths = LibraryPaths{tempDir.path()};
    auto const expected = std::array{std::byte{0x10}, std::byte{0x20}, std::byte{0x30}};
    auto const resourceId = writeResource(tempDir.path(), paths.databasePath(), expected);
    installCacheEntry(tempDir.path() / "cache", expected);
    auto executorPtr = std::make_unique<QueuedExecutor>();
    auto* const executor = executorPtr.get();
    auto runtimePtr = std::make_unique<AppRuntime>(ao::test::requireValue(AppRuntime::create(AppRuntimeDependencies{
      .executorPtr = std::move(executorPtr),
      .musicRoot = tempDir.path(),
      .databasePath = paths.databasePath(),
      .cacheDirectory = tempDir.path() / "cache",
      .musicLibraryPinnedMapBytes = library::test::kTestMusicLibraryMapBytes,
      .workspaceConfigStorePtr = std::make_unique<ConfigStore>(tempDir.path() / "workspace.yaml"),
    })));
    auto received = std::vector<std::byte>{};
    bool callbackOnExecutor = false;
    auto request = runtimePtr->resourceBytes().request(resourceId,
                                                       [&](ResourceBytes bytes)
                                                       {
                                                         received.assign(bytes.view().begin(), bytes.view().end());
                                                         callbackOnExecutor = executor->isCurrent();
                                                       });

    REQUIRE(request);
    REQUIRE(executor->drainUntil([&] { return received.size() == expected.size(); }));
    CHECK(received == std::vector<std::byte>(expected.begin(), expected.end()));
    CHECK(callbackOnExecutor);

    runtimePtr->async().requestStop();
    runtimePtr->async().join();
  }

  TEST_CASE("ResourceByteMemoryCache - equal requests share one read and cache immutable bytes",
            "[runtime][unit][resource-byte][concurrency]")
  {
    auto owner = RuntimeOwner{};
    auto release = AsyncBarrier{};
    auto readCount = AsyncTestState<std::size_t>::create(0);
    auto const expected = std::vector{std::byte{0x31}, std::byte{0x32}};
    auto cache = ResourceByteMemoryCache{
      owner.runtimePtr()->async(), std::bind_front(readAfterRelease, readCount, &release, expected)};
    auto callbackOrder = std::vector<std::int32_t>{};
    auto received = std::vector<std::vector<std::byte>>{};
    auto first = cache.request(ResourceId{7},
                               [&](ResourceBytes bytes)
                               {
                                 callbackOrder.push_back(1);
                                 received.emplace_back(bytes.view().begin(), bytes.view().end());
                               });
    REQUIRE(first);
    REQUIRE(readCount.waitUntil(1));
    auto second = cache.request(ResourceId{7},
                                [&](ResourceBytes bytes)
                                {
                                  callbackOrder.push_back(2);
                                  received.emplace_back(bytes.view().begin(), bytes.view().end());
                                });
    REQUIRE(second);
    CHECK(readCount.load() == 1);

    release.release();
    REQUIRE(owner.executor().drainUntil([&] { return received.size() == 2; }));
    CHECK(callbackOrder == std::vector<std::int32_t>{1, 2});
    CHECK(received == std::vector<std::vector<std::byte>>{expected, expected});

    auto cached = cache.request(ResourceId{7},
                                [&](ResourceBytes bytes)
                                {
                                  callbackOrder.push_back(3);
                                  received.emplace_back(bytes.view().begin(), bytes.view().end());
                                });
    CHECK_FALSE(cached);
    CHECK(readCount.load() == 1);
    CHECK(callbackOrder == std::vector<std::int32_t>{1, 2, 3});
    CHECK(received.back() == expected);
  }

  TEST_CASE("ResourceByteMemoryCache - entry budget evicts the least recently used id",
            "[runtime][unit][resource-byte][concurrency]")
  {
    auto owner = RuntimeOwner{};
    auto readCount = AsyncTestState<std::size_t>::create(0);
    auto cache =
      ResourceByteMemoryCache{owner.runtimePtr()->async(), std::bind_front(readSizedBytes, readCount, false), 2, 100};

    CHECK(requestAndWait(cache, owner, ResourceId{1}).size() == 2);
    CHECK(requestAndWait(cache, owner, ResourceId{2}).size() == 2);
    CHECK(requestAndWait(cache, owner, ResourceId{1}).size() == 2);
    CHECK(readCount.load() == 2);

    CHECK(requestAndWait(cache, owner, ResourceId{3}).size() == 2);
    CHECK(requestAndWait(cache, owner, ResourceId{2}).size() == 2);
    CHECK(readCount.load() == 4);

    bool invalidCompleted = false;
    auto invalidRequest = cache.request(kInvalidResourceId, [&](ResourceBytes) { invalidCompleted = true; });
    CHECK_FALSE(invalidRequest);
    CHECK_FALSE(invalidCompleted);
  }

  TEST_CASE("ResourceByteMemoryCache - byte budget evicts entries and does not retain an oversized payload",
            "[runtime][unit][resource-byte][concurrency]")
  {
    auto owner = RuntimeOwner{};
    auto readCount = AsyncTestState<std::size_t>::create(0);
    auto cache =
      ResourceByteMemoryCache{owner.runtimePtr()->async(), std::bind_front(readSizedBytes, readCount, true), 4, 5};

    CHECK(requestAndWait(cache, owner, ResourceId{1}).size() == 2);
    CHECK(requestAndWait(cache, owner, ResourceId{2}).size() == 2);
    CHECK(requestAndWait(cache, owner, ResourceId{1}).size() == 2);
    CHECK(requestAndWait(cache, owner, ResourceId{3}).size() == 2);
    CHECK(requestAndWait(cache, owner, ResourceId{2}).size() == 2);
    CHECK(readCount.load() == 4);

    CHECK(requestAndWait(cache, owner, ResourceId{4}).size() == 6);
    CHECK(requestAndWait(cache, owner, ResourceId{4}).size() == 6);
    CHECK(readCount.load() == 6);
  }

  TEST_CASE("ResourceByteMemoryCache - zero capacities clamp to the smallest usable budgets",
            "[runtime][unit][resource-byte][concurrency]")
  {
    auto owner = RuntimeOwner{};
    auto readCount = AsyncTestState<std::size_t>::create(0);

    SECTION("zero entries retains exactly one entry")
    {
      auto cache =
        ResourceByteMemoryCache{owner.runtimePtr()->async(), std::bind_front(readSizedBytes, readCount, false), 0, 100};

      CHECK(requestAndWait(cache, owner, ResourceId{1}).size() == 2);
      CHECK(requestAndWait(cache, owner, ResourceId{2}).size() == 2);
      CHECK(requestAndWait(cache, owner, ResourceId{1}).size() == 2);
      CHECK(readCount.load() == 3);
    }

    SECTION("zero bytes retains no payload larger than one byte")
    {
      auto cache =
        ResourceByteMemoryCache{owner.runtimePtr()->async(), std::bind_front(readSizedBytes, readCount, false), 4, 0};

      CHECK(requestAndWait(cache, owner, ResourceId{1}).size() == 2);
      CHECK(requestAndWait(cache, owner, ResourceId{1}).size() == 2);
      CHECK(readCount.load() == 2);
    }
  }

  TEST_CASE("ResourceByteMemoryCache - cached request completes synchronously",
            "[runtime][regression][resource-byte][concurrency]")
  {
    auto owner = RuntimeOwner{};
    auto const expected = std::vector{std::byte{0x23}, std::byte{0x24}};
    auto cache = ResourceByteMemoryCache{owner.runtimePtr()->async(), std::bind_front(readCustomBytes, expected)};
    auto warmedBytes = ResourceBytes{};
    auto warmRequest = cache.request(ResourceId{73}, [&](ResourceBytes bytes) { warmedBytes = std::move(bytes); });

    REQUIRE(warmRequest);
    REQUIRE(owner.executor().drainUntil([&] { return !warmedBytes.empty(); }));

    bool requestReturned = false;
    bool callbackBeforeReturn = false;
    std::size_t callbackCount = 0;
    auto retained = ResourceBytes{};
    auto cachedRequest = cache.request(ResourceId{73},
                                       [&](ResourceBytes bytes)
                                       {
                                         callbackBeforeReturn = !requestReturned;
                                         ++callbackCount;
                                         retained = std::move(bytes);
                                       });
    requestReturned = true;

    CHECK_FALSE(cachedRequest);
    CHECK(callbackBeforeReturn);
    CHECK(callbackCount == 1);
    CHECK(std::vector<std::byte>{retained.view().begin(), retained.view().end()} == expected);
  }

  TEST_CASE("ResourceByteMemoryCache - custom source delivers bytes that survive cache destruction",
            "[runtime][unit][resource-byte][concurrency]")
  {
    auto owner = RuntimeOwner{};
    auto const expected = std::vector{std::byte{0x27}, std::byte{0x28}};
    auto cachePtr = std::make_unique<ResourceByteMemoryCache>(
      owner.runtimePtr()->async(), std::bind_front(readCustomBytes, expected));
    auto retained = ResourceBytes{};
    auto request = cachePtr->request(ResourceId{72}, [&](ResourceBytes bytes) { retained = std::move(bytes); });

    REQUIRE(request);
    REQUIRE(owner.executor().drainUntil([&] { return !retained.empty(); }));
    auto const* storage = retained.view().data();
    cachePtr.reset();

    CHECK(std::vector<std::byte>{retained.view().begin(), retained.view().end()} == expected);
    CHECK(retained.view().data() == storage);
  }

  TEST_CASE("ResourceByteMemoryCache - read result failure completes empty and permits retry",
            "[runtime][regression][resource-byte][concurrency]")
  {
    auto owner = RuntimeOwner{};
    auto readCount = AsyncTestState<std::size_t>::create(0);
    auto failNextPtr = std::make_shared<std::atomic_bool>(true);
    auto const expected = std::vector{std::byte{0x41}, std::byte{0x42}};
    auto cache = ResourceByteMemoryCache{
      owner.runtimePtr()->async(), std::bind_front(readAfterOneFailure, failNextPtr, readCount, expected)};
    auto received = std::vector<std::vector<std::byte>>{};
    auto first = cache.request(
      ResourceId{8}, [&](ResourceBytes bytes) { received.emplace_back(bytes.view().begin(), bytes.view().end()); });

    REQUIRE(first);
    REQUIRE(owner.executor().drainUntil([&] { return received.size() == 1; }));
    CHECK(received.front().empty());

    auto retry = cache.request(
      ResourceId{8}, [&](ResourceBytes bytes) { received.emplace_back(bytes.view().begin(), bytes.view().end()); });
    REQUIRE(retry);
    REQUIRE(owner.executor().drainUntil([&] { return received.size() == 2; }));
    CHECK(readCount.load() == 2);
    CHECK(received.back() == expected);
  }

  TEST_CASE("ResourceByteMemoryCache - cancellation escapes without invoking a waiter",
            "[runtime][regression][resource-byte][concurrency]")
  {
    auto owner = RuntimeOwner{};
    auto readCount = AsyncTestState<std::size_t>::create(0);
    auto cache = ResourceByteMemoryCache{owner.runtimePtr()->async(), std::bind_front(cancelRead, readCount)};
    auto callbackCount = AsyncTestState<std::size_t>::create(0);
    auto request = cache.request(ResourceId{9}, [callbackCount](ResourceBytes) { callbackCount.increment(); });

    REQUIRE(request);
    REQUIRE(readCount.waitUntil(1));
    owner.runtimePtr()->async().requestStop();
    owner.runtimePtr()->async().join();
    CHECK(callbackCount.load() == 0);
  }

  TEST_CASE("ResourceByteMemoryCache - destruction fences an old flight from a same-id replacement",
            "[runtime][regression][resource-byte][concurrency]")
  {
    auto owner = RuntimeOwner{};
    auto release = AsyncBarrier{};
    auto readCount = AsyncTestState<std::size_t>::create(0);
    auto firstReadReleased = AsyncTestState<bool>::create(false);
    auto const readBytes = std::bind_front(readAcrossRebind, readCount, firstReadReleased, &release);
    auto cachePtr = std::make_unique<ResourceByteMemoryCache>(owner.runtimePtr()->async(), readBytes);
    auto callbackCount = AsyncTestState<std::size_t>::create(0);
    auto received = std::vector<std::byte>{};
    auto oldRequest = cachePtr->request(ResourceId{10}, [callbackCount](ResourceBytes) { callbackCount.increment(); });

    REQUIRE(oldRequest);
    REQUIRE(readCount.waitUntil(1));
    cachePtr.reset();
    release.release();
    REQUIRE(firstReadReleased.waitUntil(true));

    auto replacementCache = ResourceByteMemoryCache{owner.runtimePtr()->async(), readBytes};
    auto replacement = replacementCache.request(ResourceId{10},
                                                [&](ResourceBytes bytes)
                                                {
                                                  received.assign(bytes.view().begin(), bytes.view().end());
                                                  callbackCount.increment();
                                                });
    REQUIRE(replacement);
    REQUIRE(owner.executor().drainUntil([&] { return callbackCount.load() == 1; }));
    CHECK(readCount.load() == 2);
    CHECK(received == std::vector<std::byte>{std::byte{0x21}, std::byte{0x22}});

    owner.runtimePtr()->async().requestStop();
    owner.runtimePtr()->async().join();
    CHECK(callbackCount.load() == 1);
  }
} // namespace ao::rt::test
