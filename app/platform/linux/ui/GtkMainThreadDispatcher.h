// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include "core/IMainThreadDispatcher.h"
#include <glibmm/dispatcher.h>
#include <functional>
#include <mutex>
#include <vector>

namespace app::ui
{
  /**
   * @brief GTK-based implementation of IMainThreadDispatcher.
   */
  class GtkMainThreadDispatcher final : public core::IMainThreadDispatcher
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
