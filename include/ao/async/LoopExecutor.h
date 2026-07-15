// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include "QueuedExecutorBase.h"

#include <functional>
#include <semaphore>

namespace ao::async
{
  // Runs queued callback turns on the thread that constructs and drives it.
  // Foreign producers only enqueue and signal; they never execute callbacks.
  // Callback exceptions propagate to the driver; unexecuted callbacks remain
  // ready for a later turn.
  class LoopExecutor final : public QueuedExecutorBase
  {
  public:
    LoopExecutor() = default;
    ~LoopExecutor() override = default;

    LoopExecutor(LoopExecutor const&) = delete;
    LoopExecutor& operator=(LoopExecutor const&) = delete;
    LoopExecutor(LoopExecutor&&) = delete;
    LoopExecutor& operator=(LoopExecutor&&) = delete;

    // Wait for and execute one logical executor turn.
    void runOneTurn();

    // Execute one ready turn without waiting. Returns false when none is ready.
    bool runReadyTurn();

  private:
    void wake() override;
    void executeTask(std::move_only_function<void()>& task) override;

    std::binary_semaphore _wakeSignal{0};
  };
} // namespace ao::async
