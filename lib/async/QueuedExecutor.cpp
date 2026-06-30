// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/async/QueuedExecutor.h>

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
    auto tasksToRun = std::vector<std::move_only_function<void()>>{};

    {
      auto const lock = std::scoped_lock{_mutex};
      tasksToRun.swap(_tasks);
    }

    for (auto& task : tasksToRun)
    {
      if (task)
      {
        executeTask(task);
      }
    }
  }

  void QueuedExecutorBase::enqueueAndWake(std::move_only_function<void()> task)
  {
    if (!task)
    {
      return;
    }

    {
      auto const lock = std::scoped_lock{_mutex};
      _tasks.push_back(std::move(task));
    }

    wake();
  }
} // namespace ao::async
