// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/async/Executor.h>

#include <glibmm/dispatcher.h>

#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace ao::gtk
{
  class GtkMainContextExecutor final : public async::IExecutor
  {
  public:
    GtkMainContextExecutor();
    ~GtkMainContextExecutor() override = default;

    // Not copyable or movable
    GtkMainContextExecutor(GtkMainContextExecutor const&) = delete;
    GtkMainContextExecutor& operator=(GtkMainContextExecutor const&) = delete;
    GtkMainContextExecutor(GtkMainContextExecutor&&) = delete;
    GtkMainContextExecutor& operator=(GtkMainContextExecutor&&) = delete;

    bool isCurrent() const noexcept override;
    void dispatch(std::move_only_function<void()> task) override;
    void defer(std::move_only_function<void()> task) override;

  private:
    void onDispatched();
    void executeTask(std::move_only_function<void()>& task);

    Glib::Dispatcher _dispatcher;
    std::mutex _mutex;
    std::vector<std::move_only_function<void()>> _tasks;
    std::thread::id _ownerThread;
  };
} // namespace ao::gtk
