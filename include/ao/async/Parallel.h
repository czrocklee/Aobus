// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/async/Task.h>

#include <vector>

namespace ao::async
{
  class Runtime;

  /**
   * Run @p tasks concurrently on @p runtime's worker pool and complete once
   * every task has finished.
   *
   * The awaiting coroutine is suspended while the tasks run and holds no
   * worker thread, so `whenAll` cannot starve the pool: with a pool of one
   * thread the tasks simply run sequentially. If any task exits with an
   * exception, the first one in task order is rethrown after all tasks have
   * completed. Cancellation of the awaiting coroutine is forwarded to the
   * spawned tasks.
   */
  Task<> whenAll(Runtime* runtime, std::vector<Task<>> tasks);
} // namespace ao::async
