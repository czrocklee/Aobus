// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/async/Runtime.h>
#include <ao/async/Task.h>

#include <functional>
#include <stop_token>
#include <string_view>
#include <type_traits>
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
      scope,
      [runtimeHandle = &runtime, ownerHandle = &owner, workflow = std::move(workflow)](
        std::stop_token const stopToken) mutable
      { return runUiWorkflow(runtimeHandle, ownerHandle, std::move(workflow), stopToken); },
      exceptionContext);
  }

  template<typename Owner, typename Value, typename Completion>
    requires(!std::is_void_v<Value>)
  async::Task<void> completeUiTask(async::Runtime* runtime,
                                   Owner* owner,
                                   async::Task<Value> task,
                                   Completion completion,
                                   std::stop_token const stopToken)
  {
    auto completedRes = co_await std::move(task);
    co_await runtime->resumeOnCallbackExecutor(stopToken);
    std::invoke(std::move(completion), owner, std::move(completedRes));
  }

  /**
   * Starts one result-bearing task and delivers its result to the live owner.
   *
   * The caller starts the operation synchronously. Task ownership, suspension,
   * callback-executor resumption, cancellation, and exception reporting stay in
   * this workflow boundary rather than being repeated by every GTK caller.
   */
  template<typename Owner, typename Value, typename Completion>
    requires(!std::is_void_v<Value>)
  void spawnUiTask(async::Runtime& runtime,
                   async::LifetimeScope& scope,
                   Owner& owner,
                   std::string_view const exceptionContext,
                   async::Task<Value> task,
                   Completion completion)
  {
    spawnUiWorkflow(
      runtime,
      scope,
      owner,
      exceptionContext,
      [runtimeHandle = &runtime, task = std::move(task), completion = std::move(completion)](
        Owner* ownerHandle, std::stop_token const stopToken) mutable
      { return completeUiTask(runtimeHandle, ownerHandle, std::move(task), std::move(completion), stopToken); });
  }
} // namespace ao::gtk
