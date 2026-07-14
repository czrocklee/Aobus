// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/async/QueuedExecutor.h>

#include <gsl-lite/gsl-lite.hpp>

#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace ao::async
{
  QueuedExecutorBase::QueuedExecutorBase()
    : _ownerThread{std::this_thread::get_id()}
  {
  }

  bool QueuedExecutorBase::isCurrent() const noexcept
  {
    return std::this_thread::get_id() == _ownerThread;
  }

  void QueuedExecutorBase::dispatch(std::move_only_function<void()> task)
  {
    if (!task)
    {
      return;
    }

    if (isCurrent())
    {
      executeTask(task);
      return;
    }

    enqueueAndWake(std::move(task));
  }

  void QueuedExecutorBase::defer(std::move_only_function<void()> task)
  {
    enqueueAndWake(std::move(task));
  }

  void QueuedExecutorBase::drainQueuedTasks()
  {
    gsl_Expects(isCurrent());

    {
      auto const lock = std::scoped_lock{_mutex};

      if (_draining)
      {
        return;
      }

      _draining = true;
      _drainTasks.swap(_pendingTasks);
    }

    auto taskException = std::exception_ptr{};

    try
    {
      for (auto& task : _drainTasks)
      {
        if (task)
        {
          executeTask(task);
        }
      }
    }
    catch (...)
    {
      taskException = std::current_exception();
    }

    _drainTasks.clear();

    bool shouldWake = false;
    {
      auto const lock = std::scoped_lock{_mutex};
      _draining = false;
      shouldWake = !_pendingTasks.empty();
    }

    if (shouldWake)
    {
      wake();
    }

    if (taskException)
    {
      std::rethrow_exception(taskException);
    }
  }

  void QueuedExecutorBase::enqueueAndWake(std::move_only_function<void()> task)
  {
    if (!task)
    {
      return;
    }

    bool shouldWake = false;
    {
      auto const lock = std::scoped_lock{_mutex};
      shouldWake = _pendingTasks.empty() && !_draining;
      _pendingTasks.push_back(std::move(task));
    }

    if (shouldWake)
    {
      wake();
    }
  }
} // namespace ao::async
