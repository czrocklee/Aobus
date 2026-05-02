// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <functional>

namespace ao
{
  /**
   * @brief Interface for dispatching tasks to the main UI thread.
   *
   * This interface allows core components to execute code on the UI thread
   * without depending on specific UI frameworks like GTK or Qt.
   */
  class IMainThreadDispatcher
  {
  public:
    virtual ~IMainThreadDispatcher() = default;

    /**
     * @brief Dispatches a task to be executed on the main UI thread.
     *
     * @param task The function object to execute.
     */
    virtual void dispatch(std::function<void()> task) = 0;
  };
} // namespace ao
