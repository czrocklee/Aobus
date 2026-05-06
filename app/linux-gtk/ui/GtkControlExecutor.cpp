// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "GtkControlExecutor.h"

namespace ao::gtk
{
  GtkControlExecutor::GtkControlExecutor()
    : _ownerThread{std::this_thread::get_id()}
  {
    _dispatcher.connect(sigc::mem_fun(*this, &GtkControlExecutor::onDispatched));
  }

  bool GtkControlExecutor::isCurrent() const noexcept
  {
    return std::this_thread::get_id() == _ownerThread;
  }

  void GtkControlExecutor::dispatch(std::move_only_function<void()> task)
  {
    if (!task)
    {
      return;
    }

    {
      std::lock_guard<std::mutex> lock(_mutex);
      _tasks.push_back(std::move(task));
    }
    _dispatcher.emit();
  }

  void GtkControlExecutor::onDispatched()
  {
    std::vector<std::move_only_function<void()>> tasksToRun;
    {
      std::lock_guard<std::mutex> lock(_mutex);
      tasksToRun.swap(_tasks);
    }

    for (auto& task : tasksToRun)
    {
      if (task)
      {
        try
        {
          task();
        }
        catch (...)
        {
          // Continue processing remaining tasks
        }
      }
    }
  }
}
