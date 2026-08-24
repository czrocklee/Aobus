// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/async/QueuedExecutorBase.h>

#include <ao/Contract.h>
#include <ao/compat/MoveOnlyFunction.h>

#include <cstddef>
#include <exception>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace ao::async
{
  namespace
  {
    constexpr std::size_t kMaxFinalDrainTurns = 1024;
  } // namespace

  QueuedExecutorBase::QueuedExecutorBase()
    : _ownerThread{std::this_thread::get_id()}
  {
  }

  bool QueuedExecutorBase::isCurrent() const noexcept
  {
    return std::this_thread::get_id() == _ownerThread;
  }

  void QueuedExecutorBase::dispatch(compat::MoveOnlyFunction<void()> task)
  {
    if (!task)
    {
      return;
    }

    if (isCurrent())
    {
      try
      {
        task();
      }
      catch (...)
      {
        AO_FATAL_EXCEPTION(std::current_exception(), "queued executor callback");
      }

      return;
    }

    enqueueAndWake(std::move(task));
  }

  void QueuedExecutorBase::defer(compat::MoveOnlyFunction<void()> task)
  {
    enqueueAndWake(std::move(task));
  }

  void QueuedExecutorBase::drainQueuedTasks()
  {
    drainQueuedTasksTurn(true);
  }

  void QueuedExecutorBase::drainQueuedTasksUntilIdle()
  {
    AO_EXPECTS(isCurrent());

    {
      auto const lock = std::scoped_lock{_mutex};
      AO_EXPECTS(!_draining, "Queued executor final drain cannot reenter an active callback turn");
    }

    for (std::size_t turn = 0; turn < kMaxFinalDrainTurns; ++turn)
    {
      if (!drainQueuedTasksTurn(false))
      {
        return;
      }
    }

    AO_FATAL("Queued executor final drain did not reach quiescence after {} turns", kMaxFinalDrainTurns);
  }

  bool QueuedExecutorBase::drainQueuedTasksTurn(bool const wakeRemaining)
  {
    AO_EXPECTS(isCurrent());

    {
      auto const lock = std::scoped_lock{_mutex};

      if (_draining)
      {
        return false;
      }

      _draining = true;

      if (_drainTasks.empty())
      {
        _drainTasks.swap(_pendingTasks);
      }
    }

    auto taskException = std::exception_ptr{};

    try
    {
      while (_nextDrainTaskIndex < _drainTasks.size())
      {
        auto task = std::move(_drainTasks[_nextDrainTaskIndex]);
        ++_nextDrainTaskIndex;

        if (task)
        {
          task();
        }
      }
    }
    catch (...)
    {
      taskException = std::current_exception();
    }

    bool shouldWake = false;
    {
      auto const lock = std::scoped_lock{_mutex};

      if (_nextDrainTaskIndex == _drainTasks.size())
      {
        _drainTasks.clear();
        _nextDrainTaskIndex = 0;
      }

      _draining = false;
      shouldWake = _nextDrainTaskIndex < _drainTasks.size() || !_pendingTasks.empty();
    }

    if (shouldWake && wakeRemaining)
    {
      wake();
    }

    if (taskException)
    {
      AO_FATAL_EXCEPTION(std::move(taskException), "queued executor callback");
    }

    return shouldWake;
  }

  void QueuedExecutorBase::enqueueAndWake(compat::MoveOnlyFunction<void()> task)
  {
    if (!task)
    {
      return;
    }

    bool shouldWake = false;
    {
      auto const lock = std::scoped_lock{_mutex};
      shouldWake = _pendingTasks.empty() && _drainTasks.empty() && !_draining;
      _pendingTasks.push_back(std::move(task));
    }

    if (shouldWake)
    {
      wake();
    }
  }
} // namespace ao::async
