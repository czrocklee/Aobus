// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "app/GtkMainContextExecutor.h"

#include <ao/Contract.h>

#include <exception>

namespace ao::gtk
{
  GtkMainContextExecutor::GtkMainContextExecutor()
  {
    _dispatcher.connect([this] { drainQueuedTasks(); });
  }

  void GtkMainContextExecutor::wake() noexcept
  {
    try
    {
      _dispatcher.emit();
    }
    catch (...)
    {
      AO_FATAL_EXCEPTION(std::current_exception(), "GTK executor wake");
    }
  }
} // namespace ao::gtk
