// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/async/LoopExecutor.h>

#include <gsl-lite/gsl-lite.hpp>

#include <functional>

namespace ao::async
{
  void LoopExecutor::runOneTurn()
  {
    gsl_Expects(isCurrent());
    _wakeSignal.acquire();
    drainQueuedTasks();
  }

  bool LoopExecutor::runReadyTurn()
  {
    gsl_Expects(isCurrent());

    if (!_wakeSignal.try_acquire())
    {
      return false;
    }

    drainQueuedTasks();
    return true;
  }

  void LoopExecutor::wake()
  {
    _wakeSignal.release();
  }

  void LoopExecutor::executeTask(std::move_only_function<void()>& task)
  {
    if (task)
    {
      task();
    }
  }
} // namespace ao::async
