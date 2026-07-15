// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "CliRuntime.h"

#include "test/unit/TestUtils.h"
#include "test/unit/library/TrackTestSupport.h"
#include <ao/Exception.h>
#include <ao/async/Executor.h>
#include <ao/async/Runtime.h>
#include <ao/async/Task.h>
#include <ao/rt/CoreRuntime.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <semaphore>
#include <sstream>
#include <thread>

namespace ao::cli::test
{
  namespace
  {
    struct ThreadHopResult final
    {
      std::thread::id workerThread;
      std::thread::id callbackThread;
    };

    async::Task<ThreadHopResult> workerRoundTrip(async::Runtime* runtime)
    {
      co_await runtime->resumeOnWorker();
      auto const workerThread = std::this_thread::get_id();
      co_await runtime->resumeOnCallbackExecutor();
      co_return ThreadHopResult{.workerThread = workerThread, .callbackThread = std::this_thread::get_id()};
    }

    async::Task<void> failOnWorker(async::Runtime* runtime)
    {
      co_await runtime->resumeOnWorker();
      throwException<Exception>("worker task failed");
    }

    async::Task<void> finishAfterOwnerRelease(async::Runtime* runtime,
                                              std::binary_semaphore* release,
                                              std::atomic_bool* completed)
    {
      co_await runtime->resumeOnWorker();
      release->acquire();
      completed->store(true, std::memory_order_release);
    }
  } // namespace

  TEST_CASE("CliRuntime - runTask returns worker completion on the CLI owner thread",
            "[cli][unit][runtime][concurrency]")
  {
    auto temp = ao::test::TempDir{};
    auto out = std::ostringstream{};
    auto err = std::ostringstream{};
    auto cli = CliRuntime{out, err, library::test::kTestMusicLibraryMapSize};
    cli.options().root = temp.path();
    auto const ownerThread = std::this_thread::get_id();
    auto& asyncRuntime = cli.core().async();

    auto const result = cli.runTask(workerRoundTrip(&asyncRuntime));

    CHECK(result.workerThread != ownerThread);
    CHECK(result.callbackThread == ownerThread);
  }

  TEST_CASE("CliRuntime - runTask rethrows worker failure on the CLI owner thread", "[cli][unit][runtime][concurrency]")
  {
    auto temp = ao::test::TempDir{};
    auto out = std::ostringstream{};
    auto err = std::ostringstream{};
    auto cli = CliRuntime{out, err, library::test::kTestMusicLibraryMapSize};
    cli.options().root = temp.path();
    auto& asyncRuntime = cli.core().async();

    CHECK_THROWS_AS(cli.runTask(failOnWorker(&asyncRuntime)), Exception);
  }

  TEST_CASE("CliRuntime - teardown drains callbacks queued by a foreign producer", "[cli][unit][runtime][concurrency]")
  {
    auto temp = ao::test::TempDir{};
    auto out = std::ostringstream{};
    auto err = std::ostringstream{};
    bool callbackRan = false;

    {
      auto cli = CliRuntime{out, err, library::test::kTestMusicLibraryMapSize};
      cli.options().root = temp.path();
      auto& callbackExecutor = cli.core().async().callbackExecutor();
      auto producer = std::jthread{[&] { callbackExecutor.dispatch([&] { callbackRan = true; }); }};
      producer.join();

      CHECK_FALSE(callbackRan);
    }

    CHECK(callbackRan);
  }

  TEST_CASE("CliRuntime - callback failure does not outlive the task boundary", "[cli][unit][runtime][concurrency]")
  {
    auto temp = ao::test::TempDir{};
    auto out = std::ostringstream{};
    auto err = std::ostringstream{};
    auto cli = CliRuntime{out, err, library::test::kTestMusicLibraryMapSize};
    cli.options().root = temp.path();
    auto& asyncRuntime = cli.core().async();
    auto& callbackExecutor = asyncRuntime.callbackExecutor();
    auto release = std::binary_semaphore{0};
    auto taskCompleted = std::atomic_bool{false};

    callbackExecutor.defer([] { throwException<Exception>("callback failed"); });
    callbackExecutor.defer([&] { release.release(); });

    CHECK_THROWS_AS(cli.runTask(finishAfterOwnerRelease(&asyncRuntime, &release, &taskCompleted)), Exception);
    CHECK(taskCompleted.load(std::memory_order_acquire));
  }
} // namespace ao::cli::test
