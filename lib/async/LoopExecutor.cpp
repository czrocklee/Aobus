// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/async/LoopExecutor.h>

#include <ao/Contract.h>

namespace ao::async
{
  void LoopExecutor::runOneTurn()
  {
    AO_EXPECTS(isCurrent());
    _wakeSignal.acquire();
    drainQueuedTasks();
  }

  bool LoopExecutor::runReadyTurn()
  {
    AO_EXPECTS(isCurrent());

    if (!_wakeSignal.try_acquire())
    {
      return false;
    }

    drainQueuedTasks();
    return true;
  }

  void LoopExecutor::wake() noexcept
  {
    _wakeSignal.release();
  }
} // namespace ao::async
