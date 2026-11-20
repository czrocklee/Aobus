// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "CliRuntime.h"

#include "test/TestFixtureSupport.h"
#include "test/library/MusicLibraryTestSupport.h"
#include <ao/async/Executor.h>
#include <ao/async/OperationCancelled.h>
#include <ao/async/Runtime.h>
#include <ao/async/Task.h>
#include <ao/rt/CoreRuntime.h>
#include <ao/rt/library/LibraryDurability.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <memory>
#include <sstream>
#include <stdexcept>
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

    async::Task<ThreadHopResult> workerRoundTripAsync(async::Runtime* runtime)
    {
      co_await runtime->resumeOnWorkerAsync();
      auto const workerThread = std::this_thread::get_id();
      co_await runtime->resumeOnCallbackExecutorAsync();
      co_return ThreadHopResult{.workerThread = workerThread, .callbackThread = std::this_thread::get_id()};
    }

    async::Task<void> failOnWorkerAsync(async::Runtime* runtime)
    {
      co_await runtime->resumeOnWorkerAsync();
      throw std::runtime_error{"worker task failed"};
    }

    async::Task<void> markTaskStartedAsync(std::shared_ptr<std::atomic_bool> startedPtr)
    {
      startedPtr->store(true);
      co_return;
    }
  } // namespace

  TEST_CASE("CliRuntime - runTask returns worker completion on the CLI owner thread", "[concurrency]")
  {
    auto temp = ao::test::TempDir{};
    auto out = std::ostringstream{};
    auto err = std::ostringstream{};
    auto cli = CliRuntime{out, err, library::test::kTestMusicLibraryMapBytes};
    cli.options().root = temp.path();
    auto const ownerThread = std::this_thread::get_id();
    auto& asyncRuntime = cli.core().async();

    auto const result = cli.runTask(workerRoundTripAsync(&asyncRuntime));

    CHECK(result.workerThread != ownerThread);
    CHECK(result.callbackThread == ownerThread);
  }

  TEST_CASE("CliRuntime - runTask rethrows worker failure on the CLI owner thread", "[concurrency]")
  {
    auto temp = ao::test::TempDir{};
    auto out = std::ostringstream{};
    auto err = std::ostringstream{};
    auto cli = CliRuntime{out, err, library::test::kTestMusicLibraryMapBytes};
    cli.options().root = temp.path();
    auto& asyncRuntime = cli.core().async();

    CHECK_THROWS_AS(cli.runTask(failOnWorkerAsync(&asyncRuntime)), std::runtime_error);
  }

  TEST_CASE("CliRuntime - runTask throws cancellation after an explicit stop")
  {
    auto temp = ao::test::TempDir{};
    auto out = std::ostringstream{};
    auto err = std::ostringstream{};
    auto startedPtr = std::make_shared<std::atomic_bool>(false);
    auto cli = CliRuntime{
      out, err, library::test::kTestMusicLibraryMapBytes, rt::LibraryDurability::Durable, temp.path() / "cache"};
    cli.options().root = temp.path();
    cli.core().async().requestStop();

    CHECK_THROWS_AS(cli.runTask(markTaskStartedAsync(startedPtr)), async::OperationCancelled);
    CHECK_FALSE(startedPtr->load());
  }

  TEST_CASE("CliRuntime - teardown drains callbacks queued by a foreign producer", "[concurrency]")
  {
    auto temp = ao::test::TempDir{};
    auto out = std::ostringstream{};
    auto err = std::ostringstream{};
    bool callbackRan = false;

    {
      auto cli = CliRuntime{out, err, library::test::kTestMusicLibraryMapBytes};
      cli.options().root = temp.path();
      auto& callbackExecutor = cli.core().async().callbackExecutor();
      auto producer = std::jthread{[&] { callbackExecutor.dispatch([&] { callbackRan = true; }); }};
      producer.join();

      CHECK_FALSE(callbackRan);
    }

    CHECK(callbackRan);
  }
} // namespace ao::cli::test
