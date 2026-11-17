// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <memory>

namespace ao::async
{
  class Runtime;

  // Owns cancellation for tasks admitted by Runtime::spawnWithLifetime().
  // Destruction calls cancelAll(); keep the scope alive during concurrent calls that use it.
  class [[nodiscard]] LifetimeScope final
  {
  public:
    LifetimeScope();
    ~LifetimeScope();

    LifetimeScope(LifetimeScope const&) = delete;
    LifetimeScope& operator=(LifetimeScope const&) = delete;
    LifetimeScope(LifetimeScope&&) = delete;
    LifetimeScope& operator=(LifetimeScope&&) = delete;

    // Permanently closes admission, detaches registered tasks, and requests their
    // cooperative stop. A racing spawn either registers before closure or returns
    // without submitting work or invoking its task factory. Stop callbacks may run
    // synchronously. Repeated calls are no-ops; this does not join tasks.
    void cancelAll();

    // Reports whether the scope still tracks any tasks, not whether previously
    // admitted tasks have finished. In particular, closing the scope detaches them.
    bool empty() const;

  private:
    struct State;

    std::shared_ptr<State> _statePtr;

    friend class Runtime;
  };
} // namespace ao::async
