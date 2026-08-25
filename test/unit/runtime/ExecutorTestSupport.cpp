// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/runtime/ExecutorTestSupport.h"

#include <ao/async/LoopExecutor.h>
#include <ao/compat/MoveOnlyFunction.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace ao::rt::test
{
  struct ManualExecutor::Impl final
  {
    mutable std::mutex mutex;
    std::deque<compat::MoveOnlyFunction<void()>> tasks;
    mutable std::condition_variable cv;
    std::thread::id ownerThread = std::this_thread::get_id();
  };

  ManualExecutor::ManualExecutor()
    : _implPtr{std::make_unique<Impl>()}
  {
  }

  ManualExecutor::~ManualExecutor() = default;

  bool ManualExecutor::isCurrent() const noexcept
  {
    return std::this_thread::get_id() == _implPtr->ownerThread;
  }

  void ManualExecutor::dispatch(compat::MoveOnlyFunction<void()> task)
  {
    {
      auto const lock = std::scoped_lock{_implPtr->mutex};
      _implPtr->tasks.push_back(std::move(task));
    }

    _implPtr->cv.notify_all();
  }

  void ManualExecutor::defer(compat::MoveOnlyFunction<void()> task)
  {
    dispatch(std::move(task));
  }

  bool ManualExecutor::runOne()
  {
    if (!isCurrent())
    {
      throw std::runtime_error{"ManualExecutor can only be drained on its owner thread"};
    }

    auto task = compat::MoveOnlyFunction<void()>{};

    {
      auto const lock = std::scoped_lock{_implPtr->mutex};

      if (_implPtr->tasks.empty())
      {
        return false;
      }

      task = std::move(_implPtr->tasks.front());
      _implPtr->tasks.pop_front();
    }

    task();
    return true;
  }

  void ManualExecutor::runUntilIdle()
  {
    while (runOne())
    {
    }
  }

  std::size_t ManualExecutor::queuedCount() const
  {
    auto const lock = std::scoped_lock{_implPtr->mutex};
    return _implPtr->tasks.size();
  }

  bool ManualExecutor::waitUntilQueued(std::chrono::milliseconds const timeout) const
  {
    auto lock = std::unique_lock{_implPtr->mutex};
    return _implPtr->cv.wait_for(lock, timeout, [this] { return !_implPtr->tasks.empty(); });
  }

  bool ManualExecutor::waitUntilQueuedCount(std::size_t const expected, std::chrono::milliseconds const timeout) const
  {
    auto lock = std::unique_lock{_implPtr->mutex};
    return _implPtr->cv.wait_for(lock, timeout, [this, expected] { return _implPtr->tasks.size() >= expected; });
  }

  void ManualExecutor::checkQueued(std::chrono::milliseconds const timeout) const
  {
    INFO("Timed out waiting for queued executor task");
    REQUIRE(waitUntilQueued(timeout));
  }

  InlineExecutor::InlineExecutor() noexcept
    : _ownerThread{std::this_thread::get_id()}
  {
  }

  bool InlineExecutor::isCurrent() const noexcept
  {
    return std::this_thread::get_id() == _ownerThread;
  }

  void InlineExecutor::dispatch(compat::MoveOnlyFunction<void()> task)
  {
    execute(std::move(task));
  }

  void InlineExecutor::defer(compat::MoveOnlyFunction<void()> task)
  {
    execute(std::move(task));
  }

  void InlineExecutor::execute(compat::MoveOnlyFunction<void()> task) const
  {
    if (!task)
    {
      return;
    }

    if (!isCurrent())
    {
      throw std::runtime_error{"InlineExecutor can only execute work on its owner thread"};
    }

    task();
  }

  struct QueuedExecutor::Impl final
  {
    mutable std::mutex mutex;
    mutable std::condition_variable cv;
    std::size_t queuedCount = 0;
    async::LoopExecutor loopExecutor;
  };

  QueuedExecutor::QueuedExecutor()
    : _implPtr{std::make_unique<Impl>()}
  {
  }

  QueuedExecutor::~QueuedExecutor() = default;

  bool QueuedExecutor::isCurrent() const noexcept
  {
    return _implPtr->loopExecutor.isCurrent();
  }

  void QueuedExecutor::dispatch(compat::MoveOnlyFunction<void()> task)
  {
    enqueue(std::move(task));
  }

  void QueuedExecutor::defer(compat::MoveOnlyFunction<void()> task)
  {
    enqueue(std::move(task));
  }

  bool QueuedExecutor::runReadyTurn()
  {
    return _implPtr->loopExecutor.runReadyTurn();
  }

  void QueuedExecutor::drain()
  {
    while (runReadyTurn())
    {
    }
  }

  std::size_t QueuedExecutor::queuedCount() const
  {
    auto const lock = std::scoped_lock{_implPtr->mutex};
    return _implPtr->queuedCount;
  }

  bool QueuedExecutor::waitUntilQueued(std::chrono::milliseconds const timeout) const
  {
    auto lock = std::unique_lock{_implPtr->mutex};
    return _implPtr->cv.wait_for(lock, timeout, [this] { return _implPtr->queuedCount != 0; });
  }

  bool QueuedExecutor::waitUntilQueuedCount(std::size_t const expected, std::chrono::milliseconds const timeout) const
  {
    auto lock = std::unique_lock{_implPtr->mutex};
    return _implPtr->cv.wait_for(lock, timeout, [this, expected] { return _implPtr->queuedCount >= expected; });
  }

  void QueuedExecutor::checkQueued(std::chrono::milliseconds const timeout) const
  {
    INFO("Timed out waiting for queued executor task");
    REQUIRE(waitUntilQueued(timeout));
  }

  void QueuedExecutor::enqueue(compat::MoveOnlyFunction<void()> task)
  {
    if (!task)
    {
      return;
    }

    {
      auto const lock = std::scoped_lock{_implPtr->mutex};
      ++_implPtr->queuedCount;

      try
      {
        _implPtr->loopExecutor.defer(
          [this, task = std::move(task)] mutable
          {
            {
              auto const taskLock = std::scoped_lock{_implPtr->mutex};
              --_implPtr->queuedCount;
            }

            task();
          });
      }
      catch (...)
      {
        --_implPtr->queuedCount;
        throw;
      }
    }

    _implPtr->cv.notify_all();
  }

  bool runLoopUntil(async::LoopExecutor& executor,
                    compat::MoveOnlyFunction<bool()> predicate,
                    std::chrono::milliseconds const timeout)
  {
    auto const deadline = std::chrono::steady_clock::now() + timeout;

    while (!predicate() && std::chrono::steady_clock::now() < deadline)
    {
      if (!executor.runReadyTurn())
      {
        std::this_thread::yield();
      }
    }

    return predicate();
  }
} // namespace ao::rt::test
