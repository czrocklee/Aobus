// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/async/Runtime.h>
#include <ao/async/Task.h>

#include <stop_token>
#include <string_view>
#include <utility>

namespace ao::async
{
  class LifetimeScope;
}

namespace ao::gtk
{
  /**
   * Starts a lifetime-bound workflow whose entry runs on the callback executor.
   *
   * The workflow may hop to worker executors internally, but it must not touch GTK objects or owner state from
   * worker-side code. Cancellation remains control-flow: it is rethrown to LifetimeScope and handled silently.
   */
  template<typename Owner, typename Workflow>
  async::Task<void> runUiWorkflow(async::Runtime* runtime,
                                  Owner* owner,
                                  Workflow workflow,
                                  std::stop_token const stopToken)
  {
    co_await runtime->resumeOnCallbackExecutor(stopToken);
    co_await workflow(owner, stopToken);
  }

  template<typename Owner, typename Workflow>
  void spawnUiWorkflow(async::Runtime& runtime,
                       async::LifetimeScope& scope,
                       Owner& owner,
                       std::string_view const exceptionContext,
                       Workflow workflow)
  {
    runtime.spawnWithLifetime(
      &scope,
      [runtimeHandle = &runtime, ownerHandle = &owner, workflow = std::move(workflow)](
        std::stop_token const stopToken) mutable
      { return runUiWorkflow(runtimeHandle, ownerHandle, std::move(workflow), stopToken); },
      exceptionContext);
  }
} // namespace ao::gtk
