// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/RuntimeTestUtils.h"
#include <ao/Exception.h>
#include <ao/async/ImmediateExecutor.h>
#include <ao/async/Runtime.h>
#include <ao/async/Task.h>
#include <ao/rt/CorePrimitives.h>

#include <catch2/catch_test_macros.hpp>
#include <fcntl.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

namespace ao::rt::test
{
  using namespace ao::async;

  namespace
  {
    Task<std::thread::id> pingPongTask(Runtime* runtime, AsyncTestState<int> counter)
    {
      co_await runtime->resumeOnWorker();
      // Now on worker thread — the thread switch is the behavior under test.
      (*counter)++;

      co_await runtime->resumeOnCallbackExecutor();
      // Now back on UI (ImmediateExecutor for tests)
      (*counter)++;

      co_return std::this_thread::get_id();
    }

    Task<void> failingTask(Runtime* runtime)
    {
      co_await runtime->resumeOnWorker();
      throwException<Exception>("Test failure");
    }

    /// RAII guard that redirects stderr to /dev/null for its lifetime.
    /// Used to silence the intentional "unhandled exception" diagnostic that
    /// spawnLogged emits when exercising the exception-logging path.
    class StderrSilencer final
    {
    public:
      StderrSilencer()
        : _savedFd{::dup(STDERR_FILENO)}
      {
        std::fflush(stderr);
        int const devNull = ::open("/dev/null", O_WRONLY);
        ::dup2(devNull, STDERR_FILENO);
        ::close(devNull);
      }

      ~StderrSilencer()
      {
        std::fflush(stderr);
        ::dup2(_savedFd, STDERR_FILENO);
        ::close(_savedFd);
      }

      StderrSilencer(StderrSilencer const&) = delete;
      StderrSilencer(StderrSilencer&&) = delete;
      StderrSilencer& operator=(StderrSilencer const&) = delete;
      StderrSilencer& operator=(StderrSilencer&&) = delete;

    private:
      int _savedFd{-1};
    };
  } // namespace

  TEST_CASE("AsyncRuntime - spawn switches to worker and returns through callback executor", "[runtime][unit][async]")
  {
    auto executor = ImmediateExecutor{};
    auto runtime = Runtime{executor};
    auto counter = AsyncTestState<int>::create(0);

    auto future = runtime.spawn(pingPongTask(&runtime, counter));
    auto const result = future.get();

    CHECK(result != std::this_thread::get_id());
    CHECK(counter.get() == 2);

    runtime.requestStop();
    runtime.join();
  }

  TEST_CASE("AsyncRuntime - spawn reports task failures through logging and futures", "[runtime][unit][async]")
  {
    auto executor = ImmediateExecutor{};
    auto runtime = Runtime{executor};

    {
      // The logging path intentionally writes a diagnostic to stderr from a
      // worker thread; silence it until join() guarantees the handler has run.
      auto const silencer = StderrSilencer{};

      // Logging version - should not crash
      CHECK_NOTHROW(runtime.spawnLogged(failingTask(&runtime)));

      // Future version - should throw when getting result
      auto future = runtime.spawn(failingTask(&runtime));
      CHECK_THROWS_AS(future.get(), Exception);

      runtime.requestStop();
      runtime.join();
    }
  }

  TEST_CASE("ImmediateExecutor - defer is FIFO and never reenters the current task", "[runtime][unit][async]")
  {
    auto executor = ImmediateExecutor{};
    auto order = std::vector<int>{};

    executor.defer(
      [&]
      {
        order.push_back(1);
        executor.defer(
          [&]
          {
            order.push_back(3);
            executor.defer([&] { order.push_back(5); });
          });
        executor.defer([&] { order.push_back(4); });
        order.push_back(2);
      });

    CHECK(order == std::vector<int>{1, 2, 3, 4, 5});

    executor.dispatch([&] { order.push_back(6); });
    CHECK(order.back() == 6);
  }

  TEST_CASE("ImmediateExecutor - a throwing task does not wedge the queue", "[runtime][unit][async]")
  {
    auto executor = ImmediateExecutor{};
    auto order = std::vector<int>{};

    CHECK_THROWS_AS(executor.defer(
                      [&]
                      {
                        executor.defer([&] { order.push_back(1); });
                        throwException<Exception>("boom");
                      }),
                    Exception);

    // The task deferred before the throw stayed queued and runs in the next turn.
    CHECK(order.empty());
    executor.defer([&] { order.push_back(2); });
    CHECK(order == std::vector<int>{1, 2});
  }

  TEST_CASE("Signal - re-posting during a posted emission runs after the current emission",
            "[runtime][unit][async][signal]")
  {
    auto executor = ImmediateExecutor{};
    auto signal = Signal<std::int32_t>{};
    auto order = std::vector<std::int32_t>{};

    auto subscription = signal.connect(
      [&](std::int32_t value)
      {
        order.push_back(value);

        if (value == 1)
        {
          signal.post(executor, 2);
          // The re-posted emission must not run inline inside this handler.
          order.push_back(-1);
        }
      });

    signal.post(executor, 1);

    CHECK(order == std::vector<std::int32_t>{1, -1, 2});
  }

  TEST_CASE("Signal - handlers connected during emission join the next emission", "[runtime][unit][async][signal]")
  {
    auto signal = Signal<std::int32_t>{};
    auto order = std::vector<std::int32_t>{};
    auto subscriptions = std::vector<Subscription>{};

    subscriptions.push_back(signal.connect(
      [&](std::int32_t value)
      {
        order.push_back(value);

        if (value == 1)
        {
          // Enough connects to force handler-vector reallocation while the emit loop is live.
          for (std::int32_t added = 0; added < 16; ++added)
          {
            subscriptions.push_back(signal.connect([&](std::int32_t inner) { order.push_back(100 + inner); }));
          }
        }
      }));

    signal.emit(1);
    CHECK(order == std::vector<std::int32_t>{1});

    signal.emit(2);
    REQUIRE(order.size() == 1 + 1 + 16);
    CHECK(order[1] == 2);
    CHECK(order.back() == 102);
  }
} // namespace ao::rt::test
