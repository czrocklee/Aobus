// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/async/Sleeper.h>
#include <ao/async/Task.h>
#include <ao/async/TaskFuture.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace ao::rt::test
{
  template<typename T>
  std::exception_ptr captureTaskFutureException(async::TaskFuture<T>& future)
  {
    try
    {
      if constexpr (std::is_void_v<T>)
      {
        future.get();
      }
      else
      {
        std::ignore = future.get();
      }
    }
    catch (...)
    {
      // Keep ownership explicit while tests inspect a cross-thread exception;
      // GCC ThreadSanitizer cannot model ownership held by an active catch.
      return std::current_exception();
    }

    return {};
  }

  // Injectable delay strategy for deterministic Runtime tests. The sleeper
  // must outlive every Runtime that receives it.
  class ControlledSleeper final : public async::Sleeper
  {
  public:
    using Delay = std::chrono::milliseconds;

    struct Call final
    {
      std::uint64_t id = 0;
      Delay delay{};
      bool cancelled = false;
      std::thread::id startedOn;
      std::thread::id cancelledOn;
    };

    ControlledSleeper();
    ~ControlledSleeper() override;

    ControlledSleeper(ControlledSleeper const&) = delete;
    ControlledSleeper& operator=(ControlledSleeper const&) = delete;
    ControlledSleeper(ControlledSleeper&&) = delete;
    ControlledSleeper& operator=(ControlledSleeper&&) = delete;

    async::Task<void> sleepFor(Delay delay, std::stop_token stopToken) override;
    bool waitForCallCount(std::size_t count, std::chrono::milliseconds timeout = std::chrono::seconds{2}) const;
    std::size_t callCount() const;
    Call call(std::size_t index) const;
    bool waitForCancellation(std::size_t index, std::chrono::milliseconds timeout = std::chrono::seconds{2}) const;
    bool fire(std::size_t index);
    bool fireNext();
    bool fireNext(Delay delay);
    bool fireById(std::uint64_t id);
    std::uint64_t lastScheduledId() const;
    std::vector<Delay> pendingDelays() const;
    bool waitForPendingDelays(std::vector<Delay> const& expected,
                              std::chrono::milliseconds timeout = std::chrono::seconds{2}) const;
    bool waitForPendingDelay(Delay delay, std::chrono::milliseconds timeout = std::chrono::seconds{2}) const;

  private:
    struct Impl;
    std::unique_ptr<Impl> _implPtr;
  };

  /**
   * @brief Lifecycle-safe test state tracker.
   */
  template<typename T>
  class AsyncTestState final
  {
  public:
    static auto create(T initial) { return AsyncTestState{std::make_shared<Data>(initial)}; }

    void set(T value) const
    {
      // A waiter may destroy the object containing this AsyncTestState as soon
      // as it observes the value. Keep notify under the lock so wait_for cannot
      // return until this method has made its final access through `this`.
      auto const lock = std::scoped_lock{_dataPtr->mutex};
      _dataPtr->value.store(value);
      _dataPtr->cv.notify_all();
    }

    T increment() const
    {
      T result = {};

      // See set() for the lifetime synchronization provided by the lock.
      auto const lock = std::scoped_lock{_dataPtr->mutex};
      result = _dataPtr->value.fetch_add(1) + 1;
      _dataPtr->cv.notify_all();
      return result;
    }

    T load() const { return _dataPtr->value.load(); }

    bool waitUntil(T expected, std::chrono::milliseconds timeout = std::chrono::seconds{2}) const
    {
      auto lock = std::unique_lock{_dataPtr->mutex};
      return _dataPtr->cv.wait_for(lock, timeout, [this, expected] { return load() == expected; });
    }

  private:
    struct Data final
    {
      explicit Data(T initial)
        : value{initial}
      {
      }

      std::atomic<T> value;
      std::mutex mutex;
      std::condition_variable cv;
    };

    explicit AsyncTestState(std::shared_ptr<Data> dataPtr)
      : _dataPtr{std::move(dataPtr)}
    {
    }

    std::shared_ptr<Data> _dataPtr;
  };

  class AsyncBarrier final
  {
  public:
    AsyncBarrier();
    ~AsyncBarrier();

    AsyncBarrier(AsyncBarrier const&) = delete;
    AsyncBarrier& operator=(AsyncBarrier const&) = delete;
    AsyncBarrier(AsyncBarrier&&) = delete;
    AsyncBarrier& operator=(AsyncBarrier&&) = delete;

    void wait();
    void release();

  private:
    struct Impl;
    std::unique_ptr<Impl> _implPtr;
  };

  namespace detail
  {
    class TaskCompletionFlag final
    {
    public:
      explicit TaskCompletionFlag(std::shared_ptr<std::atomic_bool> completedPtr)
        : _completedPtr{std::move(completedPtr)}
      {
      }

      ~TaskCompletionFlag() noexcept { _completedPtr->store(true); }

      TaskCompletionFlag(TaskCompletionFlag const&) = delete;
      TaskCompletionFlag& operator=(TaskCompletionFlag const&) = delete;
      TaskCompletionFlag(TaskCompletionFlag&&) = delete;
      TaskCompletionFlag& operator=(TaskCompletionFlag&&) = delete;

    private:
      std::shared_ptr<std::atomic_bool> _completedPtr;
    };
  } // namespace detail

  // The RAII flag also completes when Runtime teardown destroys a suspended
  // coroutine frame instead of resuming it through its normal return path.
  template<typename T>
  async::Task<T> flagCompletion(std::shared_ptr<std::atomic_bool> completedPtr, async::Task<T> task)
  {
    [[maybe_unused]] auto completionFlag = detail::TaskCompletionFlag{std::move(completedPtr)};

    if constexpr (std::is_void_v<T>)
    {
      co_await std::move(task);
      co_return;
    }
    else
    {
      co_return co_await std::move(task);
    }
  }

  template<typename RuntimeType, typename ExecutorType, typename T>
  T runQueuedTask(RuntimeType& runtime, ExecutorType& executor, async::Task<T> task)
  {
    auto completedPtr = std::make_shared<std::atomic_bool>(false);
    auto future = runtime.spawn(flagCompletion(completedPtr, std::move(task)));
    REQUIRE(executor.drainUntil([&completedPtr] { return completedPtr->load(); }));

    if constexpr (std::is_void_v<T>)
    {
      future.get();
      executor.drain();
    }
    else
    {
      auto result = future.get();
      executor.drain();
      return result;
    }
  }
} // namespace ao::rt::test
