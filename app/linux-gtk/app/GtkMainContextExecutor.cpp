// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "app/GtkMainContextExecutor.h"

#include <ao/Exception.h>
#include <ao/utility/Log.h>

#include <glibmm/main.h>
#include <sigc++/functors/mem_fun.h>

#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace ao::gtk
{
  GtkMainContextExecutor::GtkMainContextExecutor()
    : _ownerThread{std::this_thread::get_id()}
  {
    _dispatcher.connect(sigc::mem_fun(*this, &GtkMainContextExecutor::onDispatched));
  }

  bool GtkMainContextExecutor::isCurrent() const noexcept
  {
    return std::this_thread::get_id() == _ownerThread;
  }

  void GtkMainContextExecutor::dispatch(std::move_only_function<void()> task)
  {
    if (!task)
    {
      return;
    }

    {
      auto const lock = std::scoped_lock{_mutex};
      _tasks.push_back(std::move(task));
    }

    _dispatcher.emit();
  }

  void GtkMainContextExecutor::defer(std::move_only_function<void()> task)
  {
    auto sharedTaskPtr = std::make_shared<std::move_only_function<void()>>(std::move(task));
    Glib::signal_idle().connect(
      [sharedTaskPtr]
      {
        if (*sharedTaskPtr)
        {
          (*sharedTaskPtr)();
        }

        return false;
      });
  }

  void GtkMainContextExecutor::executeTask(std::move_only_function<void()>& task)
  {
    try
    {
      task();
    }
    catch (ao::Exception const& e)
    {
      APP_LOG_CRITICAL(
        "GtkMainContextExecutor: Task threw an internal exception: {} (at {}:{})", e.what(), e.file(), e.line());
    }
    catch (std::exception const& e)
    {
      APP_LOG_ERROR("GtkMainContextExecutor: Task threw an exception: {}", e.what());
    }
    catch (...)
    {
      APP_LOG_ERROR("GtkMainContextExecutor: Task threw an unknown exception");
    }
  }

  void GtkMainContextExecutor::onDispatched()
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
}
