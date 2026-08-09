// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/async/QueuedExecutorBase.h>

#include <glibmm/dispatcher.h>

namespace ao::gtk
{
  class GtkMainContextExecutor final : public async::QueuedExecutorBase
  {
  public:
    GtkMainContextExecutor();
    ~GtkMainContextExecutor() override = default;

    // Not copyable or movable
    GtkMainContextExecutor(GtkMainContextExecutor const&) = delete;
    GtkMainContextExecutor& operator=(GtkMainContextExecutor const&) = delete;
    GtkMainContextExecutor(GtkMainContextExecutor&&) = delete;
    GtkMainContextExecutor& operator=(GtkMainContextExecutor&&) = delete;

  private:
    void wake() noexcept override;

    Glib::Dispatcher _dispatcher;
  };
} // namespace ao::gtk
