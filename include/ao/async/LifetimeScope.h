// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <memory>
#include <mutex>
#include <vector>

namespace ao::async
{
  class Runtime;
  struct LifetimeScopeTask;

  struct LifetimeScopeState final
  {
    std::mutex mutex;
    std::vector<std::shared_ptr<LifetimeScopeTask>> tasks;
    bool isAlive{true};
  };

  class [[nodiscard]] LifetimeScope final
  {
  public:
    LifetimeScope();
    ~LifetimeScope();

    LifetimeScope(LifetimeScope const&) = delete;
    LifetimeScope& operator=(LifetimeScope const&) = delete;
    LifetimeScope(LifetimeScope&&) = delete;
    LifetimeScope& operator=(LifetimeScope&&) = delete;

    void cancelAll();
    std::shared_ptr<LifetimeScopeState> state() const noexcept;

  private:
    std::shared_ptr<LifetimeScopeState> _statePtr;
  };
} // namespace ao::async
