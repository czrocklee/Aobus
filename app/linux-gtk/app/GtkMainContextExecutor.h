// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/async/QueuedExecutor.h>

#include <glibmm/dispatcher.h>

#include <functional>

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
    void wake() override;
    void executeTask(std::move_only_function<void()>& task) override;

    Glib::Dispatcher _dispatcher;
  };
} // namespace ao::gtk
