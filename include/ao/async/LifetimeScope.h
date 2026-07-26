// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <memory>

namespace ao::async
{
  class Runtime;

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
    bool empty() const;

  private:
    struct State;

    std::shared_ptr<State> _statePtr;

    friend class Runtime;
  };
} // namespace ao::async
