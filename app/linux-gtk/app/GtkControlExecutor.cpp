// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "app/GtkControlExecutor.h"
#include <ao/utility/Log.h>

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
      std::lock_guard lock(_mutex);
      _tasks.push_back(std::move(task));
    }

    _dispatcher.emit();
  }

  void GtkControlExecutor::defer(std::move_only_function<void()> task)
  {
    auto sharedTask = std::make_shared<std::move_only_function<void()>>(std::move(task));
    Glib::signal_idle().connect(
      [sharedTask]
      {
        if (*sharedTask)
        {
          (*sharedTask)();
        }
        return false;
      });
  }

  void GtkControlExecutor::onDispatched()
  {
    std::vector<std::move_only_function<void()>> tasksToRun;
    {
      std::lock_guard lock(_mutex);
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
        catch (std::exception const& e)
        {
          APP_LOG_ERROR("GtkControlExecutor: Task threw an exception: {}", e.what());
        }
        catch (...)
        {
          APP_LOG_ERROR("GtkControlExecutor: Task threw an unknown exception");
        }
      }
    }
  }
}
