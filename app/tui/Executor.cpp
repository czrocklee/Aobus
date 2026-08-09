// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "Executor.h"

#include <ao/Contract.h>

#include <ftxui/component/screen_interactive.hpp>

#include <exception>

namespace ao::tui
{
  Executor::Executor(ftxui::ScreenInteractive& screen)
    : _screen{screen}
  {
  }

  void Executor::wake() noexcept
  {
    try
    {
      _screen.Post([this] { drainQueuedTasks(); });
    }
    catch (...)
    {
      AO_FATAL_EXCEPTION(std::current_exception(), "TUI executor wake");
    }
  }

  void Executor::drainPendingTasks()
  {
    drainQueuedTasks();
  }
} // namespace ao::tui
