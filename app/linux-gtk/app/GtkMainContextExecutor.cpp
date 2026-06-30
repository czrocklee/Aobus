// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "app/GtkMainContextExecutor.h"

#include <ao/Exception.h>
#include <ao/rt/Log.h>

#include <exception>
#include <functional>

namespace ao::gtk
{
  GtkMainContextExecutor::GtkMainContextExecutor()
  {
    _dispatcher.connect([this] { drainQueuedTasks(); });
  }

  void GtkMainContextExecutor::wake()
  {
    _dispatcher.emit();
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
} // namespace ao::gtk
