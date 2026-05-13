// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <functional>
#include <glibmm/dispatcher.h>
#include <mutex>
#include <runtime/CorePrimitives.h>
#include <thread>
#include <vector>

namespace ao::gtk
{
  class GtkControlExecutor final : public rt::IControlExecutor
  {
  public:
    GtkControlExecutor();
    ~GtkControlExecutor() override = default;

    bool isCurrent() const noexcept override;
    void dispatch(std::move_only_function<void()> task) override;
    void defer(std::move_only_function<void()> task) override;

  private:
    void onDispatched();

    Glib::Dispatcher _dispatcher;
    std::mutex _mutex;
    std::vector<std::move_only_function<void()>> _tasks;
    std::thread::id _ownerThread;
  };
}
