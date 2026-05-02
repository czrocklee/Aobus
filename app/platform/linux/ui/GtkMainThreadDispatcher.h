// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <ao/utility/IMainThreadDispatcher.h>
#include <functional>
#include <glibmm/dispatcher.h>
#include <mutex>
#include <vector>

namespace app::ui
{
  /**
   * @brief GTK-based implementation of ao::IMainThreadDispatcher.
   */
  class GtkMainThreadDispatcher final : public ao::IMainThreadDispatcher
  {
  public:
    GtkMainThreadDispatcher();
    ~GtkMainThreadDispatcher() override = default;

    void dispatch(std::function<void()> task) override;

  private:
    void onDispatched();

    Glib::Dispatcher _dispatcher;
    std::mutex _mutex;
    std::vector<std::function<void()>> _tasks;
  };
} // namespace app::ui
