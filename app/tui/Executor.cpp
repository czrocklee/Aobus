// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "Executor.h"

#include <ao/Exception.h>
#include <ao/rt/Log.h>

#include <ftxui/component/screen_interactive.hpp>

#include <exception>
#include <functional>

namespace ao::tui
{
  Executor::Executor(ftxui::ScreenInteractive& screen)
    : _screen{screen}
  {
  }

  void Executor::wake()
  {
    _screen.Post([this] { drainQueuedTasks(); });
  }

  void Executor::drainPendingTasks()
  {
    drainQueuedTasks();
  }

  void Executor::executeTask(std::move_only_function<void()>& task)
  {
    try
    {
      task();
    }
    catch (ao::Exception const& e)
    {
      APP_LOG_CRITICAL("Executor: task threw an internal exception: {} (at {}:{})", e.what(), e.file(), e.line());
    }
    catch (std::exception const& e)
    {
      APP_LOG_ERROR("Executor: task threw an exception: {}", e.what());
    }
    catch (...)
    {
      APP_LOG_ERROR("Executor: task threw an unknown exception");
    }
  }
} // namespace ao::tui
