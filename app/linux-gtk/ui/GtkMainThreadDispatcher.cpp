// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "GtkMainThreadDispatcher.h"

namespace ao::gtk
{
  GtkMainThreadDispatcher::GtkMainThreadDispatcher()
  {
    _dispatcher.connect(sigc::mem_fun(*this, &GtkMainThreadDispatcher::onDispatched));
  }

  void GtkMainThreadDispatcher::dispatch(std::function<void()> task)
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

  void GtkMainThreadDispatcher::onDispatched()
  {
    std::vector<std::function<void()>> tasksToRun;
    {
      std::lock_guard<std::mutex> lock(_mutex);
      tasksToRun.swap(_tasks);
    }

    for (auto const& task : tasksToRun)
    {
      if (task)
      {
        task();
      }
    }
  }
} // namespace ao::gtk
