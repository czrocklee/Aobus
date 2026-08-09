// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include "QueuedExecutorBase.h"

#include <semaphore>

namespace ao::async
{
  // Runs queued callback turns on the thread that constructs and drives it.
  // Foreign producers only enqueue and signal; they never execute callbacks.
  // The QueuedExecutorBase owner completes queue bookkeeping and enters the AO
  // fatal backend if a callback exception escapes.
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
    void wake() noexcept override;

    std::binary_semaphore _wakeSignal{0};
  };
} // namespace ao::async
