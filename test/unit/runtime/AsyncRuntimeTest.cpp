// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/RuntimeTestSupport.h"
#include <ao/Exception.h>
#include <ao/async/ImmediateExecutor.h>
#include <ao/async/Runtime.h>
#include <ao/async/Task.h>
#include <ao/rt/Signal.h>
#include <ao/rt/Subscription.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <gsl-lite/gsl-lite.hpp>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
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
      counter.increment();

      co_await runtime->resumeOnCallbackExecutor();
      // Now back on UI (ImmediateExecutor for tests)
      counter.increment();

      co_return std::this_thread::get_id();
    }

    Task<void> failingTask(Runtime* runtime)
    {
      co_await runtime->resumeOnWorker();
      throwException<Exception>("Test failure");
    }

    Task<void> failingCancellableTask(Runtime* runtime, std::stop_token const stopToken)
    {
      co_await runtime->resumeOnWorker(stopToken);
      throwException<Exception>("Test cancellable failure");
    }

    Task<void> sleepAndRecord(Runtime* runtime,
                              std::chrono::milliseconds const delay,
                              AsyncTestState<std::uint32_t> callbackCount,
                              AsyncTestState<bool> ranOnWorker,
                              std::stop_token const stopToken)
    {
      co_await runtime->sleepFor(delay, stopToken);
      ranOnWorker.set(!runtime->callbackExecutor().isCurrent());
      callbackCount.increment();
    }

    class TaskExitRecorder final
    {
    public:
      explicit TaskExitRecorder(AsyncTestState<std::uint32_t> exitCount)
        : _exitCount{std::move(exitCount)}
      {
      }

      ~TaskExitRecorder() { _exitCount.increment(); }

      TaskExitRecorder(TaskExitRecorder const&) = delete;
      TaskExitRecorder& operator=(TaskExitRecorder const&) = delete;
      TaskExitRecorder(TaskExitRecorder&&) = delete;
      TaskExitRecorder& operator=(TaskExitRecorder&&) = delete;

    private:
      AsyncTestState<std::uint32_t> _exitCount;
    };

    Task<void> timedCancellationRace(Runtime* runtime,
                                     AsyncTestState<std::uint32_t> startedCount,
                                     AsyncTestState<std::uint32_t> exitCount,
                                     std::stop_token const stopToken)
    {
      auto const exitRecorder = TaskExitRecorder{exitCount};
      startedCount.increment();
      co_await runtime->sleepFor(std::chrono::milliseconds{1}, stopToken);
    }

#ifdef _WIN32
    std::int32_t fileDescriptor(std::FILE* stream)
    {
      return ::_fileno(stream);
    }

    std::int32_t duplicateFileDescriptor(std::int32_t const fd)
    {
      return ::_dup(fd);
    }

    std::int32_t duplicateFileDescriptorTo(std::int32_t const source, std::int32_t const target)
    {
      return ::_dup2(source, target);
    }

    void closeFileDescriptor(std::int32_t const fd)
    {
      std::ignore = ::_close(fd);
    }
#else
    std::int32_t fileDescriptor(std::FILE* stream)
    {
      return ::fileno(stream);
    }

    std::int32_t duplicateFileDescriptor(std::int32_t const fd)
    {
      return ::dup(fd);
    }

    std::int32_t duplicateFileDescriptorTo(std::int32_t const source, std::int32_t const target)
    {
      return ::dup2(source, target);
    }

    void closeFileDescriptor(std::int32_t const fd)
    {
      std::ignore = ::close(fd);
    }
#endif

    struct FileCloser final
    {
      void operator()(gsl_lite::owner<std::FILE*> file) const noexcept { std::ignore = std::fclose(file); }
    };

    class StderrCapture final
    {
    public:
      StderrCapture()
        : _filePtr{std::tmpfile()}
      {
        if (_filePtr == nullptr)
        {
          throwException<Exception>("Failed to create stderr capture file");
        }

        _stderrFd = fileDescriptor(stderr);
        _savedFd = duplicateFileDescriptor(_stderrFd);

        if (_stderrFd < 0 || _savedFd < 0)
        {
          closeCaptureFile();
          throwException<Exception>("Failed to duplicate stderr");
        }

        std::ignore = std::fflush(stderr);

        if (duplicateFileDescriptorTo(fileDescriptor(_filePtr.get()), _stderrFd) < 0)
        {
          closeFileDescriptor(_savedFd);
          _savedFd = -1;
          closeCaptureFile();
          throwException<Exception>("Failed to redirect stderr");
        }
      }

      ~StderrCapture()
      {
        std::ignore = std::fflush(stderr);

        if (_savedFd >= 0)
        {
          std::ignore = duplicateFileDescriptorTo(_savedFd, _stderrFd);
          closeFileDescriptor(_savedFd);
        }

        closeCaptureFile();
      }

      StderrCapture(StderrCapture const&) = delete;
      StderrCapture& operator=(StderrCapture const&) = delete;
      StderrCapture(StderrCapture&&) = delete;
      StderrCapture& operator=(StderrCapture&&) = delete;

      std::string output()
      {
        std::ignore = std::fflush(stderr);

        if (std::fseek(_filePtr.get(), 0, SEEK_SET) != 0)
        {
          throwException<Exception>("Failed to rewind stderr capture");
        }

        auto result = std::string{};
        auto buffer = std::array<char, 256>{};

        while (true)
        {
          auto const count = std::fread(buffer.data(), sizeof(char), buffer.size(), _filePtr.get());

          if (count == 0)
          {
            break;
          }

          result.append(buffer.data(), count);
        }

        if (std::ferror(_filePtr.get()) != 0)
        {
          throwException<Exception>("Failed to read stderr capture");
        }

        return result;
      }

    private:
      void closeCaptureFile() noexcept { _filePtr.reset(); }

      std::unique_ptr<std::FILE, FileCloser> _filePtr;
      std::int32_t _stderrFd = -1;
      std::int32_t _savedFd = -1;
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
    CHECK(counter.load() == 2);

    runtime.requestStop();
    runtime.join();
  }

  TEST_CASE("AsyncRuntime - unobserved and future task failures have one owner", "[runtime][unit][async]")
  {
    auto executor = ImmediateExecutor{};
    auto exceptionRecorder = AsyncExceptionRecorder{};
    auto runtime = Runtime{executor, exceptionRecorder.handler()};

    runtime.spawnLogged(failingTask(&runtime));

    auto future = runtime.spawn(failingTask(&runtime));
    REQUIRE_THROWS_AS(future.get(), Exception);
    REQUIRE(exceptionRecorder.waitForCount(1));

    runtime.requestStop();
    runtime.join();

    requireSingleRecordedException<Exception>(exceptionRecorder, "root coroutine");
  }

  TEST_CASE("AsyncRuntime - missing exception handler uses the stderr fallback", "[runtime][unit][async]")
  {
    auto capture = StderrCapture{};
    auto executor = ImmediateExecutor{};
    auto runtime = Runtime{executor};

    CHECK_NOTHROW(runtime.reportUnhandledException(
      std::make_exception_ptr(std::runtime_error{"fallback failure"}), "fallback boundary"));

    CHECK_THAT(capture.output(),
               Catch::Matchers::ContainsSubstring("Unhandled exception in fallback boundary: fallback failure"));
  }

  TEST_CASE("AsyncRuntime - throwing exception handler falls back without escaping", "[runtime][regression][async]")
  {
    auto capture = StderrCapture{};
    auto executor = ImmediateExecutor{};
    bool handlerCalled = false;
    auto runtime = Runtime{executor,
                           [&handlerCalled](std::exception_ptr, std::string_view)
                           {
                             handlerCalled = true;
                             throwException<Exception>("diagnostic handler failure");
                           }};

    CHECK_NOTHROW(runtime.reportUnhandledException(std::make_exception_ptr(42), "throwing handler boundary"));

    CHECK(handlerCalled);
    CHECK_THAT(
      capture.output(), Catch::Matchers::ContainsSubstring("Unhandled unknown exception in throwing handler boundary"));
  }

  TEST_CASE("AsyncRuntime - cancellable task failure reaches the injected handler",
            "[runtime][unit][async][concurrency]")
  {
    auto executor = ImmediateExecutor{};
    auto exceptionRecorder = AsyncExceptionRecorder{};
    auto runtime = Runtime{executor, exceptionRecorder.handler()};

    auto task = runtime.spawnCancellable([&runtime](std::stop_token const stopToken)
                                         { return failingCancellableTask(&runtime, stopToken); });

    REQUIRE(exceptionRecorder.waitForCount(1));
    runtime.requestStop();
    runtime.join();

    requireSingleRecordedException<Exception>(exceptionRecorder, "cancellable coroutine");
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

  TEST_CASE("AsyncRuntime - sleep resumes its coroutine on the worker executor", "[runtime][unit][async]")
  {
    auto executor = QueuedExecutor{};
    auto runtime = Runtime{executor, 1};
    auto callbackCount = AsyncTestState<std::uint32_t>::create(0);
    auto ranOnWorker = AsyncTestState<bool>::create(false);

    auto task = runtime.spawnCancellable(
      [&runtime, callbackCount, ranOnWorker](std::stop_token const stopToken)
      { return sleepAndRecord(&runtime, std::chrono::milliseconds{1}, callbackCount, ranOnWorker, stopToken); });

    REQUIRE(callbackCount.waitUntil(1));
    CHECK(ranOnWorker.load());
    CHECK(executor.queuedCount() == 0);
  }

  TEST_CASE("AsyncRuntime - sleeping coroutine observes a thread-safe stop request",
            "[runtime][regression][async][concurrency]")
  {
    auto executor = ManualExecutor{};
    auto sleeper = ControlledSleeper{};
    auto exceptionRecorder = AsyncExceptionRecorder{};
    auto runtime = Runtime{executor, 1, exceptionRecorder.handler(), &sleeper};
    auto callbackCount = AsyncTestState<std::uint32_t>::create(0);
    auto ranOnWorker = AsyncTestState<bool>::create(false);

    auto task = runtime.spawnCancellable(
      [&runtime, callbackCount, ranOnWorker](std::stop_token const stopToken)
      { return sleepAndRecord(&runtime, std::chrono::seconds{30}, callbackCount, ranOnWorker, stopToken); });
    REQUIRE(sleeper.waitForCallCount(1));
    auto const sleepingCall = sleeper.call(0);
    auto const cancellingThread = std::this_thread::get_id();

    task.reset();
    REQUIRE(sleeper.waitForCancellation(0));
    auto const cancelledCall = sleeper.call(0);
    CHECK(cancelledCall.cancelled);
    CHECK(cancelledCall.startedOn != cancellingThread);
    CHECK(cancelledCall.cancelledOn == cancellingThread);
    CHECK_FALSE(sleeper.fireById(sleepingCall.id));
    runtime.requestStop();
    runtime.join();

    CHECK(callbackCount.load() == 0);
    CHECK(exceptionRecorder.snapshot().empty());
  }

  TEST_CASE("AsyncRuntime - timer expiry races safely with cancellation",
            "[runtime][regression][async][concurrency][stress]")
  {
    constexpr std::uint32_t kIterationCount = 64;
    auto executor = ManualExecutor{};
    auto runtime = Runtime{executor, 4};
    auto startedCount = AsyncTestState<std::uint32_t>::create(0);
    auto exitCount = AsyncTestState<std::uint32_t>::create(0);

    for (std::uint32_t iteration = 0; iteration < kIterationCount; ++iteration)
    {
      auto task =
        runtime.spawnCancellable([&runtime, startedCount, exitCount](std::stop_token const stopToken)
                                 { return timedCancellationRace(&runtime, startedCount, exitCount, stopToken); });
      REQUIRE(startedCount.waitUntil(iteration + 1));

      auto cancellingThread = std::jthread{[task = std::move(task)] mutable
                                           {
                                             std::this_thread::sleep_for(std::chrono::milliseconds{1});
                                             task.reset();
                                           }};
      cancellingThread.join();
      REQUIRE(exitCount.waitUntil(iteration + 1));
    }

    runtime.requestStop();
    runtime.join();
    CHECK(exitCount.load() == kIterationCount);
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

  TEST_CASE("Signal - self-unsubscribe keeps the active handler alive", "[runtime][unit][async][signal]")
  {
    auto signal = Signal<std::int32_t>{};
    auto order = std::vector<std::int32_t>{};
    auto selfSubscription = Subscription{};

    selfSubscription = signal.connect(
      [&](std::int32_t value)
      {
        order.push_back(value);
        selfSubscription.reset();
        order.push_back(-value);
      });

    auto otherSubscription = signal.connect([&](std::int32_t value) { order.push_back(100 + value); });

    signal.emit(1);
    signal.emit(2);

    CHECK(order == std::vector<std::int32_t>{1, -1, 101, 102});
  }

  TEST_CASE("Signal - throwing handler does not pin disconnected slots", "[runtime][unit][async][signal]")
  {
    auto signal = Signal<std::int32_t>{};
    auto selfSubscription = Subscription{};
    auto tokenPtr = std::make_shared<std::int32_t>(1);
    auto weakTokenPtr = std::weak_ptr<std::int32_t>{tokenPtr};

    selfSubscription = signal.connect(
      [&, tokenPtr = std::move(tokenPtr)](std::int32_t)
      {
        [[maybe_unused]] auto const& retainedTokenPtr = tokenPtr;
        selfSubscription.reset();
        throwException<Exception>("boom");
      });

    CHECK_THROWS_AS(signal.emit(1), Exception);
    CHECK(weakTokenPtr.expired());

    std::int32_t calls = 0;
    auto nextSubscription = signal.connect([&](std::int32_t) { ++calls; });

    signal.emit(2);

    CHECK(calls == 1);
  }

  TEST_CASE("Signal - throwing handler does not starve later observers", "[runtime][regression][async][signal]")
  {
    auto signal = Signal<std::int32_t>{};
    auto observed = std::vector<std::int32_t>{};
    auto firstSubscription = signal.connect([&](std::int32_t value) { observed.push_back(value); });
    auto throwingSubscription =
      signal.connect([&](std::int32_t) { throwException<Exception>("first observer failure"); });
    auto laterSubscription = signal.connect([&](std::int32_t value) { observed.push_back(value * 10); });

    CHECK_THROWS_WITH(signal.emit(2), "first observer failure");
    CHECK(observed == std::vector<std::int32_t>{2, 20});
  }

  TEST_CASE("Signal - handler can destroy owner and subscriptions can outlive it",
            "[runtime][regression][signal][lifecycle]")
  {
    auto signalPtr = std::make_unique<Signal<std::int32_t>>();
    auto order = std::vector<std::int32_t>{};
    auto firstSubscription = Subscription{};
    auto secondSubscription = Subscription{};

    firstSubscription = signalPtr->connect(
      [&](std::int32_t value)
      {
        order.push_back(value);
        firstSubscription.reset();
        signalPtr.reset();
      });
    secondSubscription = signalPtr->connect([&](std::int32_t value) { order.push_back(100 + value); });

    auto* const signal = signalPtr.get();
    signal->emit(1);

    CHECK(signalPtr == nullptr);
    CHECK(order == std::vector<std::int32_t>{1});

    // The remaining handle keeps only a weak State reference after its owner is
    // gone, so late teardown is a no-op rather than a call through a dangling
    // Signal pointer.
    secondSubscription.reset();
  }

  TEST_CASE("Signal - posted emission is cancelled when owner is destroyed", "[runtime][regression][signal][lifecycle]")
  {
    auto executor = ManualExecutor{};
    auto signalPtr = std::make_unique<Signal<std::int32_t>>();
    std::int32_t calls = 0;
    auto subscription = signalPtr->connect([&](std::int32_t) { ++calls; });

    signalPtr->post(executor, 1);
    signalPtr.reset();
    subscription.reset();
    executor.runUntilIdle();

    CHECK(calls == 0);
  }
} // namespace ao::rt::test
