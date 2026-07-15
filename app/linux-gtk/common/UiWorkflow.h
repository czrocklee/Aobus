// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/async/LifetimeScope.h>
#include <ao/async/OperationCancelled.h>
#include <ao/async/Runtime.h>
#include <ao/async/Task.h>

#include <exception>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>

namespace ao::gtk
{
  /**
   * Starts a lifetime-bound workflow whose entry and exception presentation run on the callback executor.
   *
   * The workflow may hop to worker executors internally, but it must not touch GTK objects or owner state from
   * worker-side code. Cancellation remains control-flow: it is rethrown to LifetimeScope and handled silently.
   */
  template<typename Owner, typename Workflow, typename ExceptionHandler>
  async::Task<void> runUiWorkflow(async::Runtime* runtime,
                                  Owner* owner,
                                  std::string exceptionContext,
                                  Workflow workflow,
                                  ExceptionHandler exceptionHandler,
                                  std::stop_token const stopToken)
  {
    co_await runtime->resumeOnCallbackExecutor(stopToken);

    bool failed = false;

    try
    {
      co_await workflow(owner, stopToken);
    }
    catch (...)
    {
      async::rethrowIfOperationCancelled();
      runtime->reportUnhandledException(std::current_exception(), exceptionContext);
      failed = true;
    }

    if (!failed)
    {
      co_return;
    }

    co_await runtime->resumeOnCallbackExecutor(stopToken);
    exceptionHandler(owner);
  }

  template<typename Owner, typename Workflow, typename ExceptionHandler>
  void spawnUiWorkflow(async::Runtime& runtime,
                       async::LifetimeScope& scope,
                       Owner& owner,
                       std::string_view const exceptionContext,
                       Workflow workflow,
                       ExceptionHandler exceptionHandler)
  {
    runtime.spawnWithLifetime(&scope,
                              [runtimeHandle = &runtime,
                               ownerHandle = &owner,
                               exceptionContext = std::string{exceptionContext},
                               workflow = std::move(workflow),
                               exceptionHandler = std::move(exceptionHandler)](std::stop_token const stopToken) mutable
                              {
                                return runUiWorkflow(runtimeHandle,
                                                     ownerHandle,
                                                     std::move(exceptionContext),
                                                     std::move(workflow),
                                                     std::move(exceptionHandler),
                                                     stopToken);
                              });
  }
} // namespace ao::gtk
